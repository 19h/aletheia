#include "pcode_lifter.hpp"

#include <ida/address.hpp>
#include <ida/database.hpp>
#include <ida/function.hpp>
#include <ida/graph.hpp>
#include <ida/instruction.hpp>
#include <ida/name.hpp>
#include <ida/type.hpp>

#include <ida.hpp>
#ifdef getenv
#undef getenv
#endif

#include "../frontend/shared_support.hpp"
#include "pcode_lowering.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>

#if ALETHEIA_HAS_PCODE
#include <sleigh/Support.h>
#include <sleigh/libsleigh.hh>
#endif

namespace aletheia {

#if ALETHEIA_HAS_PCODE

namespace {

struct ProcessorContext {
    int processor_id = 0;
    std::string processor_name;
    int bitness = 0;
    bool big_endian = false;
    std::string abi_name;
};

struct RegisterKey {
    std::uint64_t offset = 0;
    std::size_t size = 0;

    bool operator==(const RegisterKey& other) const = default;
};

struct RegisterKeyHash {
    std::size_t operator()(const RegisterKey& key) const noexcept {
        return std::hash<std::uint64_t>{}(key.offset) ^ (std::hash<std::size_t>{}(key.size) << 1);
    }
};

struct CollectedFunctionPcode {
    ida::Address function_start = 0;
    std::string arch_name;
    std::unordered_map<ida::Address, RawPcodeInstruction> instructions;
    std::unordered_map<RegisterKey, std::string, RegisterKeyHash> register_names;
    std::string register_space_name = "register";
    std::string const_space_name = "const";
    std::string default_code_space_name = "code";
    std::string default_data_space_name = "ram";
};

std::size_t infer_indirect_result_size(
    const CollectedFunctionPcode& collected,
    const PcodeArchitectureContext& architecture) {
    const auto storage_key = [](const RawPcodeVarnode& varnode) {
        return varnode.space_name + ":" + std::to_string(varnode.offset)
            + ":" + std::to_string(varnode.size);
    };
    const auto canonical_register = [&](const RawPcodeVarnode& varnode)
        -> std::optional<std::string> {
        if (varnode.space_name != architecture.register_space_name
            || !architecture.register_mapper) {
            return std::nullopt;
        }
        auto mapped = architecture.register_mapper(varnode.offset, varnode.size);
        return mapped ? std::optional<std::string>{mapped->canonical_name}
                      : std::nullopt;
    };

    std::unordered_map<std::string, std::optional<std::int64_t>> relative_offsets;
    bool x8_written = false;
    const auto relative_offset = [&](const RawPcodeVarnode& varnode)
        -> std::optional<std::int64_t> {
        auto found = relative_offsets.find(storage_key(varnode));
        if (found != relative_offsets.end()) {
            return found->second;
        }
        const auto name = canonical_register(varnode);
        if (!x8_written && name.has_value() && *name == "x8") {
            return 0;
        }
        return std::nullopt;
    };

    std::vector<ida::Address> addresses;
    addresses.reserve(collected.instructions.size());
    for (const auto& [address, _] : collected.instructions) {
        addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());

    std::size_t result_size = 0;
    for (ida::Address address : addresses) {
        const auto instruction = collected.instructions.find(address);
        if (instruction == collected.instructions.end()) continue;
        for (const RawPcodeOp& op : instruction->second.ops) {
            if (op.opcode == "STORE" && op.inputs.size() == 3) {
                if (auto offset = relative_offset(op.inputs[1]);
                    offset.has_value() && *offset >= 0 && op.inputs[2].size > 0) {
                    const std::uint64_t unsigned_offset =
                        static_cast<std::uint64_t>(*offset);
                    if (unsigned_offset <= std::numeric_limits<std::size_t>::max()
                        && op.inputs[2].size <= std::numeric_limits<std::size_t>::max()
                            - static_cast<std::size_t>(unsigned_offset)) {
                        result_size = std::max(
                            result_size,
                            static_cast<std::size_t>(unsigned_offset)
                                + op.inputs[2].size);
                    }
                }
            }

            if (!op.output.has_value()) continue;
            std::optional<std::int64_t> output_offset;
            if (op.opcode == "COPY" && op.inputs.size() == 1) {
                output_offset = relative_offset(op.inputs[0]);
            } else if ((op.opcode == "INT_ADD" || op.opcode == "INT_SUB")
                       && op.inputs.size() == 2) {
                auto base = relative_offset(op.inputs[0]);
                const RawPcodeVarnode& delta = op.inputs[1];
                if (base.has_value()
                    && delta.space_name == architecture.const_space_name
                    && delta.offset <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
                    const std::int64_t signed_delta =
                        static_cast<std::int64_t>(delta.offset);
                    if (op.opcode == "INT_ADD"
                        && *base <= std::numeric_limits<std::int64_t>::max()
                            - signed_delta) {
                        output_offset = *base + signed_delta;
                    } else if (op.opcode == "INT_SUB"
                               && *base >= std::numeric_limits<std::int64_t>::min()
                                   + signed_delta) {
                        output_offset = *base - signed_delta;
                    }
                }
            }
            relative_offsets[storage_key(*op.output)] = output_offset;
            const auto output_register = canonical_register(*op.output);
            if (output_register.has_value() && *output_register == "x8") {
                x8_written = true;
            }
        }
    }
    return result_size;
}

struct InferredHfaResult {
    std::size_t element_size = 0;
    std::size_t element_count = 0;
};

InferredHfaResult infer_hfa_result(
    const CollectedFunctionPcode& collected,
    const PcodeArchitectureContext& architecture) {
    if (!architecture.register_mapper) return {};
    std::vector<ida::Address> addresses;
    addresses.reserve(collected.instructions.size());
    for (const auto& [address, _] : collected.instructions) {
        addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());

    std::unordered_map<std::string, ida::Address> latest_writes;
    std::unordered_map<std::string, ida::Address> latest_reads;
    std::optional<InferredHfaResult> inferred;
    bool saw_return = false;
    for (ida::Address address : addresses) {
        const auto instruction = collected.instructions.find(address);
        if (instruction == collected.instructions.end()) continue;
        for (const RawPcodeOp& op : instruction->second.ops) {
            for (const RawPcodeVarnode& input : op.inputs) {
                if (input.space_name != architecture.register_space_name) continue;
                if (auto mapped = architecture.register_mapper(
                        input.offset, input.size)) {
                    latest_reads[mapped->canonical_name] = op.instruction_ea;
                }
            }
            if (op.output.has_value()
                && op.output->space_name == architecture.register_space_name) {
                if (auto mapped = architecture.register_mapper(
                        op.output->offset, op.output->size)) {
                    latest_writes[mapped->canonical_name] = op.instruction_ea;
                }
            }
            if (op.opcode != "RETURN") continue;
            saw_return = true;
            InferredHfaResult at_return;
            for (const auto& [prefix, element_size] :
                 std::array<std::pair<char, std::size_t>, 2>{
                     std::pair<char, std::size_t>{'s', 4},
                     std::pair<char, std::size_t>{'d', 8},
                 }) {
                std::size_t count = 0;
                for (std::size_t index = 0; index < 4; ++index) {
                    const std::string name = std::string(1, prefix)
                        + std::to_string(index);
                    auto write = latest_writes.find(name);
                    if (write == latest_writes.end()
                        || write->second > op.instruction_ea
                        || op.instruction_ea - write->second > 32) {
                        break;
                    }
                    auto read = latest_reads.find(name);
                    if (read != latest_reads.end() && read->second > write->second) {
                        break;
                    }
                    ++count;
                }
                if (count >= 2
                    && count * element_size
                        > at_return.element_count * at_return.element_size) {
                    at_return = InferredHfaResult{element_size, count};
                }
            }
            if (at_return.element_count < 2) return {};
            if (!inferred.has_value()) {
                inferred = at_return;
            } else if (inferred->element_size != at_return.element_size
                       || inferred->element_count != at_return.element_count) {
                return {};
            }
        }
    }
    return saw_return && inferred.has_value() ? *inferred : InferredHfaResult{};
}

std::vector<std::filesystem::path> spec_search_paths() {
    std::vector<std::filesystem::path> paths;
    if (const char* env = std::getenv("ALETHEIA_SLEIGH_SPEC_ROOT"); env && *env) {
        paths.emplace_back(env);
    }
#ifdef ALETHEIA_SLEIGH_SPEC_ROOT_DIR
    paths.emplace_back(ALETHEIA_SLEIGH_SPEC_ROOT_DIR);
#endif
    for (const auto& path : sleigh::gDefaultSearchPaths) {
        paths.push_back(path);
    }
    return paths;
}

std::optional<std::filesystem::path> find_spec_with_pspec(
    const std::string& filename) {
    auto sla_path = sleigh::FindSpecFile(filename, spec_search_paths());
    if (!sla_path.has_value()) {
        return std::nullopt;
    }
    auto pspec_path = *sla_path;
    pspec_path.replace_extension(".pspec");
    if (!std::filesystem::is_regular_file(pspec_path)) {
        return std::nullopt;
    }
    return sla_path;
}

std::optional<std::filesystem::path> find_arm64_spec_with_pspec() {
    return find_spec_with_pspec("AARCH64.sla");
}

std::optional<std::filesystem::path> find_x86_64_spec_with_pspec() {
    return find_spec_with_pspec("x86-64.sla");
}

ida::Result<ProcessorContext> build_processor_context(ida::Address function_address) {
    auto processor_id = ida::database::processor_id();
    if (!processor_id) return std::unexpected(processor_id.error());
    auto processor_name = ida::database::processor_name();
    if (!processor_name) return std::unexpected(processor_name.error());
    auto big_endian = ida::database::is_big_endian();
    if (!big_endian) return std::unexpected(big_endian.error());
    auto fn = ida::function::at(function_address);
    if (!fn) return std::unexpected(fn.error());

    ProcessorContext context;
    context.processor_id = *processor_id;
    context.processor_name = *processor_name;
    context.bitness = fn->bitness();
    context.big_endian = *big_endian;
    if (auto abi = ida::database::abi_name()) {
        context.abi_name = *abi;
    }
    return context;
}

ida::Result<std::filesystem::path> choose_pcode_spec(
    const ProcessorContext& context,
    const std::string& arch_name) {
    if (context.bitness != 64 || context.big_endian) {
        return std::unexpected(ida::Error::unsupported(
            "P-Code frontend requires a supported little-endian 64-bit target",
            context.processor_name + " bits=" + std::to_string(context.bitness)));
    }
    if (arch_name == "arm64") {
        if (auto path = find_arm64_spec_with_pspec(); path.has_value()) {
            return *path;
        }
        return std::unexpected(ida::Error::not_found(
            "Sleigh specification pair not found",
            "AARCH64.sla and adjacent AARCH64.pspec are required"));
    }
    if (arch_name == "x86_64") {
        if (auto path = find_x86_64_spec_with_pspec(); path.has_value()) {
            return *path;
        }
        return std::unexpected(ida::Error::not_found(
            "Sleigh specification pair not found",
            "x86-64.sla and adjacent x86-64.pspec are required"));
    }
    return std::unexpected(ida::Error::unsupported(
        "P-Code architecture is unsupported", arch_name));
}

void initialize_tag_tables_once() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        ghidra::AttributeId::initialize();
        ghidra::ElementId::initialize();
    });
}

void decode_processor_context(ghidra::Sleigh& engine,
                              ghidra::ContextInternal& context,
                              ghidra::DocumentStorage& storage) {
    const ghidra::Element* processor_spec = storage.getTag("processor_spec");
    if (processor_spec == nullptr) {
        return;
    }

    ghidra::XmlDecode decoder(&engine, processor_spec);
    ghidra::uint4 element_id = decoder.openElement(ghidra::ELEM_PROCESSOR_SPEC);
    for (;;) {
        ghidra::uint4 sub_id = decoder.peekElement();
        if (sub_id == 0) {
            break;
        }
        if (sub_id == ghidra::ELEM_CONTEXT_DATA) {
            context.decodeFromSpec(decoder);
            break;
        }
        decoder.openElement();
        decoder.closeElementSkipping(sub_id);
    }
    decoder.closeElement(element_id);
}

class IdaDatabaseLoadImage final : public ghidra::LoadImage {
public:
    explicit IdaDatabaseLoadImage(std::string arch_type)
        : ghidra::LoadImage("aletheia-pcode"),
          arch_type_(std::move(arch_type)) {}

    void loadFill(ghidra::uint1* ptr,
                  ghidra::int4 size,
                  const ghidra::Address& addr) override {
        const auto absolute = static_cast<std::uint64_t>(addr.getOffset());
        if (size < 0 || !ida::address::is_mapped(absolute)) {
            throw ghidra::DataUnavailError(
                "P-Code load address is not mapped: " + std::to_string(absolute));
        }
        auto bytes = ida::data::read_bytes(absolute, static_cast<ida::AddressSize>(size));
        if (!bytes || bytes->size() != static_cast<std::size_t>(size)) {
            throw ghidra::DataUnavailError(
                "P-Code load range is unavailable: " + std::to_string(absolute));
        }
        std::copy(bytes->begin(), bytes->end(), ptr);
    }

    std::string getArchType() const override { return arch_type_; }
    void adjustVma(long) override {}

private:
    std::string arch_type_;
};

struct SleighSession {
    SleighSession(const std::filesystem::path& sla_path, std::string arch_type)
        : load_image(std::move(arch_type)), engine(&load_image, &context) {
        std::istringstream sleigh_document("<sleigh>" + sla_path.string() + "</sleigh>");
        ghidra::Element* sleigh_root = storage.parseDocument(sleigh_document)->getRoot();
        storage.registerTag(sleigh_root);

        auto pspec_path = sla_path;
        pspec_path.replace_extension(".pspec");
        if (!std::filesystem::is_regular_file(pspec_path)) {
            throw ghidra::LowlevelError(
                "Required processor specification is unavailable: " + pspec_path.string());
        }
        ghidra::Element* pspec_root = storage.openDocument(pspec_path.string())->getRoot();
        storage.registerTag(pspec_root);

        engine.initialize(storage);
        engine.allowContextSet(false);
        decode_processor_context(engine, context, storage);
    }

    IdaDatabaseLoadImage load_image;
    ghidra::ContextInternal context;
    ghidra::DocumentStorage storage;
    ghidra::Sleigh engine;
};

class PcodeCollector final : public ghidra::PcodeEmit {
public:
    explicit PcodeCollector(CollectedFunctionPcode& output)
        : output_(output) {}

    void begin_instruction(ida::Address address) {
        current_address_ = address;
        current_ordinal_ = 0;
        output_.instructions[address] = RawPcodeInstruction{address, 0, {}};
    }

    void dump(const ghidra::Address&,
              ghidra::OpCode opcode,
              ghidra::VarnodeData* output,
              ghidra::VarnodeData* inputs,
              std::int32_t input_count) override {
        RawPcodeOp raw;
        raw.opcode = get_opname(opcode);
        raw.instruction_ea = current_address_;
        raw.op_ordinal = current_ordinal_++;

        auto convert = [](const ghidra::VarnodeData& varnode) {
            return RawPcodeVarnode{varnode.space->getName(), static_cast<std::uint64_t>(varnode.offset), static_cast<std::size_t>(varnode.size)};
        };

        if (output != nullptr) {
            raw.output = convert(*output);
        }
        for (std::int32_t index = 0; index < input_count; ++index) {
            raw.inputs.push_back(convert(inputs[index]));
        }

        if ((opcode == ghidra::CPUI_LOAD || opcode == ghidra::CPUI_STORE)
            && input_count > 0) {
            auto* selected_space = reinterpret_cast<ghidra::AddrSpace*>(
                static_cast<ghidra::uintp>(inputs[0].offset));
            if (selected_space != nullptr) {
                raw.memory_space_name = selected_space->getName();
                raw.memory_word_size = static_cast<std::size_t>(selected_space->getWordSize());
                // Never persist Sleigh's process-local AddrSpace pointer in the raw model.
                raw.inputs[0].offset = 0;
            }
        }

        output_.instructions[current_address_].ops.push_back(std::move(raw));
    }

private:
    CollectedFunctionPcode& output_;
    ida::Address current_address_ = 0;
    std::uint32_t current_ordinal_ = 0;
};

ida::Result<CollectedFunctionPcode> collect_function_pcode(ida::Address function_address) {
    auto processor_context = build_processor_context(function_address);
    if (!processor_context) {
        return std::unexpected(processor_context.error());
    }

    const std::string arch_name = frontend::detect_arch();
    auto sla_path = choose_pcode_spec(*processor_context, arch_name);
    if (!sla_path) {
        return std::unexpected(sla_path.error());
    }

    auto fn = ida::function::at(function_address);
    if (!fn) {
        return std::unexpected(fn.error());
    }
    const ida::Address function_start = fn->start();
    auto flowchart = ida::graph::flowchart(function_start);
    if (!flowchart) {
        return std::unexpected(flowchart.error());
    }

    try {
        initialize_tag_tables_once();
        SleighSession session(*sla_path, arch_name);
        ghidra::Sleigh& sleigh_engine = session.engine;

        CollectedFunctionPcode collected;
        collected.function_start = function_start;
        collected.arch_name = arch_name;
        collected.const_space_name = sleigh_engine.getConstantSpace()->getName();
        collected.default_code_space_name = sleigh_engine.getDefaultCodeSpace()->getName();
        if (auto* data_space = sleigh_engine.getDefaultDataSpace()) {
            collected.default_data_space_name = data_space->getName();
        }

        const std::string register_probe = arch_name == "x86_64" ? "RAX" : "x0";
        try {
            collected.register_space_name =
                sleigh_engine.getRegister(register_probe).space->getName();
        } catch (const ghidra::LowlevelError&) {
            return std::unexpected(ida::Error::unsupported(
                "Sleigh register model is incompatible",
                register_probe + " register is unavailable"));
        }

        std::map<ghidra::VarnodeData, std::string> sleigh_registers;
        sleigh_engine.getAllRegisters(sleigh_registers);
        for (const auto& [varnode, name] : sleigh_registers) {
            if (varnode.space != nullptr
                && varnode.space->getName() == collected.register_space_name
                && varnode.size > 0
                && !name.empty()) {
                collected.register_names[RegisterKey{
                    static_cast<std::uint64_t>(varnode.offset),
                    static_cast<std::size_t>(varnode.size)}] = name;
            }
        }

        PcodeCollector collector(collected);
        for (const auto& block : *flowchart) {
            for (ida::Address address = block.start; address < block.end; ) {
                collector.begin_instruction(address);
                try {
                    ghidra::Address sleigh_address(sleigh_engine.getDefaultCodeSpace(), address);
                    const ghidra::int4 decoded_length = sleigh_engine.oneInstruction(collector, sleigh_address);
                    const bool invalid_length = decoded_length <= 0
                        || (arch_name == "arm64" && decoded_length != 4)
                        || (arch_name == "x86_64" && decoded_length > 15);
                    if (invalid_length
                        || address + static_cast<ida::Address>(decoded_length) > block.end) {
                        return std::unexpected(ida::Error::unsupported(
                            "Unexpected instruction length",
                            "address=0x" + std::to_string(address)
                                + " sleigh=" + std::to_string(decoded_length)));
                    }
                    collected.instructions[address].decoded_length = static_cast<std::size_t>(decoded_length);
                    address += static_cast<ida::Address>(decoded_length);
                } catch (const ghidra::LowlevelError& error) {
                    return std::unexpected(ida::Error::unsupported(
                        "Failed to collect raw P-Code",
                        error.explain));
                }
            }
        }

        return collected;
    } catch (const ghidra::LowlevelError& error) {
        return std::unexpected(ida::Error::unsupported(
            "Sleigh initialization failed",
            error.explain));
    } catch (const std::exception& error) {
        return std::unexpected(ida::Error::internal(
            "P-Code collection failed",
            error.what()));
    }
}

PcodeArchitectureContext build_architecture_context(
    const CollectedFunctionPcode& collected,
    frontend::FrameLayout frame_layout,
    const DecompilerTask& task) {
    PcodeArchitectureContext context;
    context.arch_name = collected.arch_name;
    context.pointer_size = 8;
    context.big_endian = false;
    context.variadic_arguments_on_stack =
        collected.arch_name == "arm64" && inf_get_filetype() == f_MACHO;
    const auto identifies_macho = [](std::string name) {
        name = frontend::to_lower_ascii(std::move(name));
        return name.find("mach-o") != std::string::npos
            || name.find("macho") != std::string::npos;
    };
    if (collected.arch_name == "arm64"
        && !context.variadic_arguments_on_stack) {
        if (auto file_type = ida::database::file_type_name()) {
            context.variadic_arguments_on_stack = identifies_macho(*file_type);
        }
    }
    if (collected.arch_name == "arm64"
        && !context.variadic_arguments_on_stack) {
        if (auto loader_format = ida::database::loader_format_name()) {
            context.variadic_arguments_on_stack = identifies_macho(*loader_format);
        }
    }
    context.register_space_name = collected.register_space_name;
    context.const_space_name = collected.const_space_name;
    context.data_space_name = collected.default_data_space_name;
    context.code_space_name = collected.default_code_space_name;

    const auto register_names = collected.register_names;
    if (collected.arch_name == "x86_64") {
        context.gp_argument_registers = {
            "rdi", "rsi", "rdx", "rcx", "r8", "r9"};
        context.integer_return_registers = {"rax", "rdx"};
        context.stack_pointer_register = "rsp";
        context.frame_pointer_register = "rbp";
        context.register_mapper = [register_names](
            std::uint64_t offset,
            std::size_t size) -> std::optional<PcodeRegisterView> {
            auto found = register_names.find(RegisterKey{offset, size});
            if (found == register_names.end()) return std::nullopt;
            std::string name = frontend::to_lower_ascii(found->second);
            struct Alias {
                std::string_view name;
                std::string_view canonical;
                std::size_t canonical_size;
                bool low_bits;
                bool zero_extend;
            };
            static constexpr Alias aliases[] = {
                {"rax", "rax", 8, false, false}, {"eax", "rax", 8, true, true},
                {"ax", "rax", 8, true, false}, {"al", "rax", 8, true, false},
                {"rbx", "rbx", 8, false, false}, {"ebx", "rbx", 8, true, true},
                {"bx", "rbx", 8, true, false}, {"bl", "rbx", 8, true, false},
                {"rcx", "rcx", 8, false, false}, {"ecx", "rcx", 8, true, true},
                {"cx", "rcx", 8, true, false}, {"cl", "rcx", 8, true, false},
                {"rdx", "rdx", 8, false, false}, {"edx", "rdx", 8, true, true},
                {"dx", "rdx", 8, true, false}, {"dl", "rdx", 8, true, false},
                {"rsi", "rsi", 8, false, false}, {"esi", "rsi", 8, true, true},
                {"si", "rsi", 8, true, false}, {"sil", "rsi", 8, true, false},
                {"rdi", "rdi", 8, false, false}, {"edi", "rdi", 8, true, true},
                {"di", "rdi", 8, true, false}, {"dil", "rdi", 8, true, false},
                {"rbp", "rbp", 8, false, false}, {"ebp", "rbp", 8, true, true},
                {"bp", "rbp", 8, true, false}, {"bpl", "rbp", 8, true, false},
                {"rsp", "rsp", 8, false, false}, {"esp", "rsp", 8, true, true},
                {"sp", "rsp", 8, true, false}, {"spl", "rsp", 8, true, false},
            };
            for (const Alias& alias : aliases) {
                if (name == alias.name) {
                    return PcodeRegisterView{
                        std::string(alias.canonical), alias.canonical_size,
                        alias.low_bits, alias.zero_extend, false};
                }
            }
            if (name.size() >= 2 && name.front() == 'r'
                && std::isdigit(static_cast<unsigned char>(name[1]))) {
                std::size_t digits_end = 1;
                while (digits_end < name.size()
                       && std::isdigit(static_cast<unsigned char>(
                           name[digits_end]))) {
                    ++digits_end;
                }
                const std::string canonical = name.substr(0, digits_end);
                const std::string suffix = name.substr(digits_end);
                if (suffix.empty()) {
                    return PcodeRegisterView{
                        canonical, 8, false, false, false};
                }
                if (suffix == "d" || suffix == "w" || suffix == "b") {
                    return PcodeRegisterView{
                        canonical, 8, true, suffix == "d", false};
                }
            }
            if (name.starts_with("xmm")) {
                std::size_t digits_end = 3;
                while (digits_end < name.size()
                       && std::isdigit(static_cast<unsigned char>(
                           name[digits_end]))) {
                    ++digits_end;
                }
                if (digits_end > 3) {
                    const std::string canonical = name.substr(0, digits_end);
                    const std::string suffix = name.substr(digits_end);
                    if (suffix.empty()) {
                        return PcodeRegisterView{
                            canonical, size, false, false, false};
                    }
                    if ((suffix == "_qa" && size == 8)
                        || (suffix == "_da" && size == 4)) {
                        return PcodeRegisterView{
                            canonical, size, false, false, false};
                    }
                }
            }
            // High-byte registers are not low-bit aliases and retain an
            // independent carrier until bit-range views are modeled.
            return PcodeRegisterView{name, size, false, false, false};
        };
    } else {
    context.register_mapper = [register_names](std::uint64_t offset, std::size_t size) -> std::optional<PcodeRegisterView> {
        RegisterKey exact{offset, size};
        auto it = register_names.find(exact);
        if (it == register_names.end()) {
            // Sleigh leaves padding elements in overlaid AArch64 SVE/NEON
            // registers unnamed.  Raw P-Code nevertheless copies these
            // elements when scalar/vector writes clear upper lanes.  Give an
            // aligned subrange of a named z-register a canonical identity so
            // it aliases consistently without degrading to an opaque offset.
            for (const auto& [container, candidate_name] : register_names) {
                if (container.size <= size || container.offset > offset
                    || size > container.size
                    || offset - container.offset > container.size - size) {
                    continue;
                }
                std::string canonical_container =
                    frontend::to_lower_ascii(candidate_name);
                if (canonical_container.size() < 2
                    || canonical_container.front() != 'z'
                    || !std::all_of(
                        canonical_container.begin() + 1,
                        canonical_container.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
                    continue;
                }
                const std::uint64_t relative = offset - container.offset;
                if (relative % size != 0) {
                    continue;
                }
                return PcodeRegisterView{
                    canonical_container + "_part_" + std::to_string(relative)
                        + "_" + std::to_string(size),
                    size,
                    false,
                    false,
                    false};
            }
            return std::nullopt;
        }
        std::string name = it->second;

        name = frontend::to_lower_ascii(std::move(name));
        if (name == "fp") name = "x29";
        if (name == "lr") name = "x30";

        if (name == "xzr" || name == "wzr") {
            return PcodeRegisterView{"xzr", 8, false, false, true};
        }
        if (name == "wsp" || (name == "sp" && size == 4)) {
            return PcodeRegisterView{"sp", 8, true, true, false};
        }
        if (name == "sp") {
            return PcodeRegisterView{"sp", 8, false, false, false};
        }
        if ((name.size() >= 2 && name[0] == 'w')
            || (name.size() >= 2 && name[0] == 'x' && size == 4)) {
            if (name[0] == 'x') {
                return PcodeRegisterView{name, 8, true, true, false};
            }
            return PcodeRegisterView{"x" + name.substr(1), 8, true, true, false};
        }
        if (name.size() >= 2 && name[0] == 'x') {
            return PcodeRegisterView{name, 8, false, false, false};
        }
        return PcodeRegisterView{name, size, false, false, false};
    };

    context.register_read_slices = [register_names](
        std::uint64_t offset,
        std::size_t size) -> std::optional<std::vector<PcodeRegisterSlice>> {
        if (size != 16) {
            return std::nullopt;
        }
        auto it = register_names.find(RegisterKey{offset, size});
        if (it == register_names.end()) {
            return std::nullopt;
        }
        const std::string name = frontend::to_lower_ascii(it->second);
        if (name.size() < 2 || name.front() != 'q'
            || !std::all_of(name.begin() + 1, name.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            return std::nullopt;
        }
        if (offset > std::numeric_limits<std::uint64_t>::max() - 8) {
            return std::nullopt;
        }
        return std::vector<PcodeRegisterSlice>{{offset, 8}, {offset + 8, 8}};
    };
    }

    context.symbol_resolver = [](ida::Address address) -> std::optional<std::string> {
        if (auto name = ida::name::get(address); name && !name->empty()) {
            return frontend::sanitize_identifier(*name);
        }
        if (auto name = ida::function::name_at(address); name && !name->empty()) {
            return frontend::sanitize_identifier(*name);
        }
        return std::nullopt;
    };

    context.function_resolver = [&task](ida::Address address) -> std::optional<PcodeFunctionInfo> {
        PcodeFunctionInfo info;
        if (auto name = ida::name::get(address); name && !name->empty()) {
            info.name = frontend::sanitize_identifier(*name);
        } else if (auto name = ida::function::name_at(address); name && !name->empty()) {
            info.name = frontend::sanitize_identifier(*name);
        }

        bool recovered_type = false;
        if (auto type = ida::type::retrieve(address); type && type->is_function()) {
            TypeParser parser;
            if (auto return_type = type->function_return_type()) {
                if (auto text = return_type->to_string()) {
                    info.return_type = parser.parse(*text);
                }
            }
            if (auto arguments = type->function_argument_types()) {
                for (const auto& argument : *arguments) {
                    if (auto text = argument.to_string()) {
                        info.parameter_types.push_back(parser.parse(*text));
                    } else {
                        info.parameter_types.push_back(std::make_shared<UnknownType>());
                    }
                }
            }
            if (auto variadic = type->is_variadic_function()) {
                info.variadic = *variadic;
            }
            recovered_type = true;
            info.prototype_known = true;
        }

        if (!recovered_type) {
            if (TypePtr session_type = task.resolve_function_type(address)) {
                if (const auto* function = type_dyn_cast<FunctionTypeDef>(
                        session_type.get())) {
                    info.parameter_types.assign(
                        function->parameters().begin(),
                        function->parameters().end());
                    info.return_type = function->return_type();
                    info.variadic = function->variadic();
                    info.abi_indirect_result_size =
                        function->abi_indirect_result_size();
                    info.abi_hfa_result_element_size =
                        function->abi_hfa_result_element_size();
                    info.abi_hfa_result_count =
                        function->abi_hfa_result_count();
                    info.abi_parameter_locations.assign(
                        function->abi_parameter_locations().begin(),
                        function->abi_parameter_locations().end());
                    info.prototype_known = true;
                    recovered_type = true;
                }
            }
        }
        if (TypePtr session_type = task.resolve_function_type(address)) {
            if (const auto* function = type_dyn_cast<FunctionTypeDef>(
                session_type.get());
            function && (function->abi_indirect_result_size() > 0
                    || function->abi_hfa_result_count() > 0
                    || !function->abi_parameter_locations().empty())) {
                info.parameter_types.assign(
                    function->parameters().begin(),
                    function->parameters().end());
                info.return_type = function->return_type();
                info.variadic = function->variadic();
                info.abi_indirect_result_size =
                    function->abi_indirect_result_size();
                info.abi_hfa_result_element_size =
                    function->abi_hfa_result_element_size();
                info.abi_hfa_result_count = function->abi_hfa_result_count();
                info.abi_parameter_locations.assign(
                    function->abi_parameter_locations().begin(),
                    function->abi_parameter_locations().end());
                info.prototype_known = true;
                recovered_type = true;
            }
        }

        const std::string canonical = frontend::canonical_function_name(info.name);
        // Imported function stubs can lack the variadic bit even when their
        // canonical C ABI is known. Preserve the fixed parameters recovered
        // from IDA, but allow the lowerer to extend the call from current-block
        // argument-register evidence for the supported formatted-I/O family.
        if (canonical == "printf" || canonical == "fprintf") {
            info.variadic = true;
        }
        if (auto known_arity = frontend::known_call_min_arity(canonical)) {
            while (info.parameter_types.size() < *known_arity) {
                info.parameter_types.push_back(std::make_shared<UnknownType>());
            }
        }
        if (!info.return_type) {
            info.return_type = frontend::known_call_return_type(canonical);
        }

        if (!recovered_type && info.name.empty()
            && info.parameter_types.empty() && !info.return_type) {
            return std::nullopt;
        }
        return info;
    };

    context.stack_variable_resolver = [frame_layout = std::move(frame_layout)](
        DecompilerArena& arena,
        PcodeStackBase base,
        ida::Address instruction_ea,
        std::int64_t offset,
        std::size_t width) -> Variable* {
        if (base == PcodeStackBase::StackPointer) {
            return frontend::resolve_sp_relative_slot(
                arena, frame_layout, instruction_ea, offset, width);
        }
        if (!frame_layout.valid) {
            return nullptr;
        }

        // AArch64 establishes x29 at the saved x29/x30 record, which is
        // frame_layout.regs_size bytes below the function-entry SP. IDA's
        // frame member coordinates and resolve_sp_relative_slot use the
        // function-entry SP coordinate. Normalize raw [x29,#offset] accesses
        // into that same coordinate so SP- and FP-relative views alias.
        if (frame_layout.regs_size > static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max())
            || offset < std::numeric_limits<std::int64_t>::min()
                + static_cast<std::int64_t>(frame_layout.regs_size)) {
            return nullptr;
        }
        const std::int64_t canonical_offset = offset
            - static_cast<std::int64_t>(frame_layout.regs_size);

        std::string name;
        if (auto resolved = frontend::resolve_frame_variable(
                frame_layout, canonical_offset, width)) {
            name = *resolved;
        } else if (canonical_offset < 0) {
            name = "local_" + std::to_string(
                static_cast<std::uint64_t>(-(canonical_offset + 1)) + 1);
        } else {
            name = "arg_" + std::to_string(
                static_cast<std::uint64_t>(canonical_offset));
        }

        auto* variable = arena.create<Variable>(name, width);
        variable->set_aliased(true);
        variable->set_stack_offset(canonical_offset);
        variable->set_kind(canonical_offset >= 0
            ? VariableKind::StackArgument : VariableKind::StackLocal);
        auto exact = frame_layout.offset_to_var.find(canonical_offset);
        if (exact != frame_layout.offset_to_var.end() && exact->second.byte_size > 0) {
            variable->set_ir_type(std::make_shared<Integer>(exact->second.byte_size * 8, false));
        } else {
            variable->set_ir_type(std::make_shared<Integer>(width * 8, false));
        }
        return variable;
    };
    return context;
}

std::string abi_floating_register_name(
    const PcodeArchitectureContext& architecture,
    std::size_t index,
    std::size_t width) {
    if (architecture.arch_name == "x86_64") {
        return "xmm" + std::to_string(index);
    }
    const char prefix = width == 4 ? 's' : (width == 8 ? 'd' : 'q');
    return std::string(1, prefix) + std::to_string(index);
}

std::optional<ida::Address> conditional_target_for_block(
    const ida::graph::BasicBlock& block,
    const CollectedFunctionPcode& collected) {
    std::optional<std::pair<ida::Address, ida::Address>> latest;
    for (const auto& [address, instruction] : collected.instructions) {
        if (address < block.start || address >= block.end) {
            continue;
        }
        for (const RawPcodeOp& op : instruction.ops) {
            if (op.opcode != "CBRANCH" || op.inputs.empty()
                || op.inputs[0].space_name == collected.const_space_name) {
                continue;
            }
            if (!latest.has_value() || address >= latest->first) {
                latest = std::pair{address, static_cast<ida::Address>(op.inputs[0].offset)};
            }
        }
    }
    return latest.has_value() ? std::optional<ida::Address>(latest->second) : std::nullopt;
}

ida::Result<std::unique_ptr<ControlFlowGraph>> build_cfg_skeleton(
    DecompilerArena& arena,
    const std::vector<ida::graph::BasicBlock>& flowchart) {
    if (flowchart.empty()) {
        return std::unexpected(ida::Error::not_found(
            "IDA flowchart contains no basic blocks",
            "P-Code CFG construction"));
    }

    auto cfg = std::make_unique<ControlFlowGraph>();
    for (std::size_t index = 0; index < flowchart.size(); ++index) {
        auto* block = arena.create<BasicBlock>(index);
        cfg->add_block(block);
        if (index == 0) {
            cfg->set_entry_block(block);
        }
    }
    return cfg;
}

ida::Status connect_cfg_edges(
    DecompilerArena& arena,
    DecompilerTask& task,
    const std::vector<ida::graph::BasicBlock>& flowchart,
    const CollectedFunctionPcode& collected,
    ControlFlowGraph& cfg) {
    if (cfg.blocks().size() != flowchart.size()) {
        return std::unexpected(ida::Error::internal(
            "P-Code CFG block count differs from IDA flowchart",
            std::to_string(cfg.blocks().size()) + " != " + std::to_string(flowchart.size())));
    }

    const auto target_for_id = [&](int id) -> BasicBlock* {
        if (id < 0 || static_cast<std::size_t>(id) >= cfg.blocks().size()) {
            return nullptr;
        }
        return cfg.blocks()[static_cast<std::size_t>(id)];
    };

    for (std::size_t index = 0; index < flowchart.size(); ++index) {
        const auto& ida_block = flowchart[index];
        BasicBlock* block = cfg.blocks()[index];
        for (int successor : ida_block.successors) {
            if (target_for_id(successor) == nullptr) {
                return std::unexpected(ida::Error::internal(
                    "IDA flowchart contains an invalid successor",
                    "block=" + std::to_string(index)
                        + " successor=" + std::to_string(successor)));
            }
        }

        const bool indirect_dispatch = ida_block.successors.size() > 2
            && !block->instructions().empty()
            && isa<IndirectBranch>(block->instructions().back());
        if (ida_block.successors.size() > 2 && !indirect_dispatch) {
            return std::unexpected(ida::Error::unsupported(
                "Multi-successor block is not an indirect P-Code dispatch",
                "block=" + std::to_string(index)));
        }

        if (indirect_dispatch) {
            std::vector<int> ordered_targets;
            std::unordered_map<int, std::vector<std::int64_t>> values_by_target;
            for (std::size_t case_index = 0; case_index < ida_block.successors.size(); ++case_index) {
                const int successor = ida_block.successors[case_index];
                if (!values_by_target.contains(successor)) {
                    ordered_targets.push_back(successor);
                }
                values_by_target[successor].push_back(static_cast<std::int64_t>(case_index));
            }
            for (int successor : ordered_targets) {
                BasicBlock* target = target_for_id(successor);
                auto* edge = arena.create<SwitchEdge>(
                    block, target, std::move(values_by_target[successor]));
                block->add_successor(edge);
                target->add_predecessor(edge);
            }
            task.add_frontend_diagnostic(FrontendDiagnostic{
                FrontendDiagnosticSeverity::Warning,
                "pcode-switch-case-order",
                "Switch case values use IDA successor order because case metadata is unavailable",
                ida_block.end > ida_block.start ? ida_block.end - 1 : ida_block.start,
                0,
            });
            task.mutable_frontend_support_report().fallback_ops += 1;
            continue;
        }

        if (ida_block.successors.size() == 2) {
            const auto taken_address = conditional_target_for_block(ida_block, collected);
            if (!taken_address.has_value()) {
                return std::unexpected(ida::Error::unsupported(
                    "Conditional IDA block has no architectural P-Code CBRANCH",
                    "block=" + std::to_string(index)));
            }

            BasicBlock* true_target = nullptr;
            BasicBlock* false_target = nullptr;
            for (int successor : ida_block.successors) {
                BasicBlock* candidate = target_for_id(successor);
                const auto& candidate_block = flowchart[static_cast<std::size_t>(successor)];
                if (candidate_block.start == *taken_address) {
                    true_target = candidate;
                } else {
                    false_target = candidate;
                }
            }
            if (true_target == nullptr || false_target == nullptr || true_target == false_target) {
                return std::unexpected(ida::Error::unsupported(
                    "P-Code conditional target does not match IDA successors",
                    "target=0x" + std::to_string(*taken_address)));
            }

            auto* true_edge = arena.create<Edge>(block, true_target, EdgeType::True);
            auto* false_edge = arena.create<Edge>(block, false_target, EdgeType::False);
            block->add_successor(true_edge);
            block->add_successor(false_edge);
            true_target->add_predecessor(true_edge);
            false_target->add_predecessor(false_edge);
            continue;
        }

        if (ida_block.successors.size() == 1) {
            BasicBlock* target = target_for_id(ida_block.successors[0]);
            const bool fallthrough = flowchart[static_cast<std::size_t>(ida_block.successors[0])].start
                == ida_block.end;
            auto* edge = arena.create<Edge>(
                block, target, fallthrough ? EdgeType::Fallthrough : EdgeType::Unconditional);
            block->add_successor(edge);
            target->add_predecessor(edge);
        }
    }

    return ida::ok();
}

} // namespace

#endif

bool pcode_runtime_available() {
#if ALETHEIA_HAS_PCODE
    // Capability probing can run before IDA's kernel/database is initialized;
    // do not query processor metadata here. Architecture-specific selection
    // occurs in collect_function_pcode after the database is open.
    return find_arm64_spec_with_pspec().has_value()
        || find_x86_64_spec_with_pspec().has_value();
#else
    return false;
#endif
}

std::string pcode_runtime_unavailable_reason() {
#if ALETHEIA_HAS_PCODE
    if (!pcode_runtime_available()) {
        return "No supported Sleigh .sla/.pspec pair was found (AARCH64 or x86-64); set ALETHEIA_SLEIGH_SPEC_ROOT or build with ALETHEIA_PCODE_BUILD_SPECS=ON";
    }
    return {};
#else
    return "P-Code frontend was not built (Sleigh support unavailable)";
#endif
}

PcodeLifter::PcodeLifter(DecompilerArena& arena, idiomata::IdiomMatcher& idiom_matcher)
    : arena_(arena), idiom_matcher_(idiom_matcher) {}

void PcodeLifter::populate_task_signature(DecompilerTask& task) {
#if ALETHEIA_HAS_PCODE
    task.set_frontend_kind(FrontendKind::Pcode);
    task.clear_frontend_diagnostics();
    task.reset_frontend_support_report();
    frontend::populate_task_signature(task);
#else
    (void)task;
#endif
}

ida::Result<std::unique_ptr<ControlFlowGraph>> PcodeLifter::lift_function(
    DecompilerTask& task,
    std::vector<idiomata::IdiomTag>* idiom_tags_out) {
#if !ALETHEIA_HAS_PCODE
    (void)task;
    (void)idiom_tags_out;
    return std::unexpected(ida::Error::unsupported(
        "P-Code frontend unavailable",
        "Build was compiled without Sleigh support"));
#else
    task.clear_frontend_diagnostics();
    task.reset_frontend_support_report();
    if (idiom_tags_out != nullptr) {
        idiom_tags_out->clear();
    }

    auto collected_res = collect_function_pcode(task.function_address());
    if (!collected_res) {
        return std::unexpected(collected_res.error());
    }

    auto flowchart_res = ida::graph::flowchart(collected_res->function_start);
    if (!flowchart_res) {
        return std::unexpected(flowchart_res.error());
    }
    auto cfg_res = build_cfg_skeleton(arena_, *flowchart_res);
    if (!cfg_res) {
        return std::unexpected(cfg_res.error());
    }
    auto cfg = std::move(*cfg_res);

    auto signature_state = frontend::populate_task_signature(task);
    if (TypePtr inferred = task.resolve_function_type(task.function_address())) {
        if (const auto* session_function = type_dyn_cast<FunctionTypeDef>(
                inferred.get());
            session_function
            && (!task.function_type()
                || session_function->abi_indirect_result_size() > 0
                || session_function->abi_hfa_result_count() > 0
                || !session_function->abi_parameter_locations().empty())) {
            // Run-scoped signatures carry ABI lowering metadata that IDA's
            // source-level prototype cannot express. Prefer the enriched
            // signature on convergence passes, just as the direct-call
            // resolver does, so caller and callee use one allocation model.
            task.set_function_type(std::move(inferred));
        }
    }
    auto frame_layout = frontend::build_frame_layout(collected_res->function_start);
    auto architecture = build_architecture_context(
        *collected_res, std::move(frame_layout), task);
    if (const auto* recovered = task.function_type()
            ? type_dyn_cast<FunctionTypeDef>(task.function_type().get())
            : nullptr;
        architecture.arch_name == "x86_64"
        && recovered && recovered->parameters().empty()
        && recovered->abi_indirect_result_size() == 0
        && recovered->abi_hfa_result_count() == 0) {
        // A zero-argument auto-prototype is indistinguishable from a guessed
        // IDA type at this boundary. Re-infer it from first-read ABI evidence;
        // a true zero-argument function converges back to zero parameters.
        task.set_function_type(nullptr);
    }
    const std::size_t inferred_indirect_result_size =
        architecture.arch_name == "arm64"
        ? infer_indirect_result_size(*collected_res, architecture) : 0;
    const InferredHfaResult inferred_hfa_result =
        architecture.arch_name == "arm64"
            && inferred_indirect_result_size == 0
        ? infer_hfa_result(*collected_res, architecture)
        : InferredHfaResult{};
    const bool has_hidden_result_parameter = inferred_indirect_result_size > 0
        || inferred_hfa_result.element_count > 0;
    if (has_hidden_result_parameter) {
        if (const auto* recovered = task.function_type()
                ? type_dyn_cast<FunctionTypeDef>(task.function_type().get())
                : nullptr;
            recovered && recovered->abi_indirect_result_size() == 0
                && recovered->abi_hfa_result_count() == 0) {
            std::vector<TypePtr> lowered_parameters;
            lowered_parameters.reserve(recovered->parameters().size() + 1);
            TypePtr result_element = Integer::uint8_t();
            if (inferred_hfa_result.element_count > 0) {
                result_element = inferred_hfa_result.element_size == 4
                    ? Float::float32()
                    : (inferred_hfa_result.element_size == 8
                        ? Float::float64()
                        : std::make_shared<const Float>(128));
            }
            lowered_parameters.push_back(std::make_shared<Pointer>(
                result_element, architecture.pointer_size * 8));
            lowered_parameters.insert(
                lowered_parameters.end(),
                recovered->parameters().begin(),
                recovered->parameters().end());
            std::vector<AbiParameterLocation> lowered_locations(
                recovered->abi_parameter_locations().begin(),
                recovered->abi_parameter_locations().end());
            for (AbiParameterLocation& location : lowered_locations) {
                ++location.parameter_index;
            }
            task.set_function_type(std::make_shared<const FunctionTypeDef>(
                CustomType::void_type(),
                std::move(lowered_parameters),
                recovered->variadic(),
                inferred_indirect_result_size,
                inferred_hfa_result.element_size,
                inferred_hfa_result.element_count,
                std::move(lowered_locations)));
        }
    }
    PcodeSignatureContext signature_context;
    signature_context.parameter_register_map = signature_state.param_register_map;
    signature_context.parameter_count_hint = signature_state.current_function_param_count_hint;
    signature_context.prefers_w_arguments = signature_state.current_function_prefers_w_args;
    signature_context.prefers_w_return = signature_state.current_function_prefers_w_return;
    signature_context.abi_indirect_result_size = inferred_indirect_result_size;
    signature_context.abi_hfa_result_element_size =
        inferred_hfa_result.element_size;
    signature_context.abi_hfa_result_count = inferred_hfa_result.element_count;

    if (architecture.arch_name == "x86_64"
        && !task.function_type() && !has_hidden_result_parameter) {
        signature_context.parameter_register_map.clear();
        task.clear_parameter_registers();
    }

    if (!task.function_type() && has_hidden_result_parameter) {
        for (auto& [register_name, parameter_index] :
             signature_context.parameter_register_map) {
            if (register_name != "x8" && register_name != "w8") {
                ++parameter_index;
            }
        }
        if (inferred_indirect_result_size > 0) {
            signature_context.parameter_register_map["x8"] = 0;
        }
        task.clear_parameter_registers();
        if (inferred_indirect_result_size > 0) {
            const TypePtr result_pointer = std::make_shared<Pointer>(
                Integer::uint8_t(), architecture.pointer_size * 8);
            task.set_parameter_register("x8", DecompilerTask::ParameterInfo{
                "result", 0, result_pointer});
        } else if (inferred_hfa_result.element_count > 0) {
            TypePtr element = inferred_hfa_result.element_size == 4
                ? Float::float32()
                : (inferred_hfa_result.element_size == 8
                    ? Float::float64()
                    : std::make_shared<const Float>(128));
            task.set_parameter_register("__hfa_result", DecompilerTask::ParameterInfo{
                "result",
                0,
                std::make_shared<Pointer>(element, architecture.pointer_size * 8),
            });
        }
        for (const auto& [register_name, parameter_index] :
             signature_context.parameter_register_map) {
            if (register_name == "x8" || register_name == "w8") continue;
            TypePtr type = Integer::uint64_t();
            if (register_name.size() >= 2
                && (register_name.front() == 's'
                    || register_name.front() == 'd'
                    || register_name.front() == 'q')) {
                type = register_name.front() == 's' ? Float::float32()
                    : (register_name.front() == 'd' ? Float::float64()
                                                    : std::make_shared<const Float>(128));
            }
            task.set_parameter_register(register_name, DecompilerTask::ParameterInfo{
                "a" + std::to_string(parameter_index + 1),
                parameter_index,
                type,
            });
        }
    }

    if (!task.function_type() && architecture.arch_name == "arm64") {
        for (std::size_t index = 0; index < 8; ++index) {
            const int parameter_index = static_cast<int>(index)
                + (has_hidden_result_parameter ? 1 : 0);
            const std::string display = "arg_" + std::to_string(index);
            for (const auto& [prefix, type] : std::array<std::pair<char, TypePtr>, 3>{
                     std::pair<char, TypePtr>{'s', Float::float32()},
                     std::pair<char, TypePtr>{'d', Float::float64()},
                     std::pair<char, TypePtr>{'q', std::make_shared<const Float>(128)},
                 }) {
                const std::string register_name = std::string(1, prefix)
                    + std::to_string(index);
                signature_context.parameter_register_map[register_name] = parameter_index;
                task.set_parameter_register(register_name,
                    DecompilerTask::ParameterInfo{display, parameter_index, type});
            }
        }
    }

    if (const auto* function_type = task.function_type()
            ? type_dyn_cast<FunctionTypeDef>(task.function_type().get()) : nullptr) {
        const bool scalar_signature = std::all_of(
            function_type->parameters().begin(),
            function_type->parameters().end(),
            [](const TypePtr& type) {
                if (!type) return false;
                return type->type_kind() == TypeKind::Integer
                    || type->type_kind() == TypeKind::Float
                    || type->type_kind() == TypeKind::Pointer
                    || type->type_kind() == TypeKind::Enum;
            });
        if (scalar_signature) {
            signature_context.parameter_register_map.clear();
            task.clear_parameter_registers();
        }

        std::size_t gp_index = 0;
        std::size_t fp_index = 0;
        for (std::size_t parameter_index = 0;
             parameter_index < function_type->parameters().size();
             ++parameter_index) {
            const TypePtr& parameter_type = function_type->parameters()[parameter_index];
            if ((function_type->abi_indirect_result_size() > 0
                    || function_type->abi_hfa_result_count() > 0)
                && parameter_index == 0) {
                signature_context.abi_indirect_result_size =
                    function_type->abi_indirect_result_size();
                signature_context.abi_hfa_result_element_size =
                    function_type->abi_hfa_result_element_size();
                signature_context.abi_hfa_result_count =
                    function_type->abi_hfa_result_count();
                if (function_type->abi_indirect_result_size() > 0) {
                    signature_context.parameter_register_map["x8"] = 0;
                    task.set_parameter_register("x8", DecompilerTask::ParameterInfo{
                        "result", 0, parameter_type});
                } else {
                    task.set_parameter_register(
                        "__hfa_result",
                        DecompilerTask::ParameterInfo{"result", 0, parameter_type});
                }
                continue;
            }
            const auto explicit_location = std::find_if(
                function_type->abi_parameter_locations().begin(),
                function_type->abi_parameter_locations().end(),
                [&](const AbiParameterLocation& location) {
                    return location.parameter_index == parameter_index;
                });
            if (explicit_location
                != function_type->abi_parameter_locations().end()
                && explicit_location->storage != AbiParameterStorage::Default) {
                const std::string display = "a"
                    + std::to_string(parameter_index + 1);
                if (explicit_location->storage
                    == AbiParameterStorage::FloatingRegister) {
                    const std::size_t width = parameter_type
                        ? parameter_type->size_bytes() : 0;
                    if (explicit_location->register_index < 8
                        && (width == 4 || width == 8 || width == 16)) {
                        const std::string register_name =
                            abi_floating_register_name(
                                architecture,
                                explicit_location->register_index,
                                width);
                        signature_context.parameter_register_map[register_name]
                            = static_cast<int>(parameter_index);
                        task.set_parameter_register(
                            register_name,
                            DecompilerTask::ParameterInfo{
                                display,
                                static_cast<int>(parameter_index),
                                parameter_type,
                            });
                        fp_index = std::max(
                            fp_index, explicit_location->register_index + 1);
                    }
                } else if (explicit_location->storage
                           == AbiParameterStorage::GeneralRegister) {
                    const std::size_t index = explicit_location->register_index;
                    if (index < architecture.gp_argument_registers.size()) {
                        const std::string register_name =
                            architecture.gp_argument_registers[index];
                        signature_context.parameter_register_map[register_name]
                            = static_cast<int>(parameter_index);
                        task.set_parameter_register(register_name,
                            DecompilerTask::ParameterInfo{
                                display,
                                static_cast<int>(parameter_index),
                                parameter_type,
                            });
                        if (architecture.arch_name == "arm64"
                            && register_name.starts_with('x')) {
                            std::string narrow_name = register_name;
                            narrow_name.front() = 'w';
                            signature_context.parameter_register_map[narrow_name]
                                = static_cast<int>(parameter_index);
                            task.set_parameter_register(narrow_name,
                                DecompilerTask::ParameterInfo{
                                    display,
                                    static_cast<int>(parameter_index),
                                    parameter_type,
                                });
                        }
                        gp_index = std::max(gp_index, index + 1);
                    }
                }
                // Stack locations are annotated after lowering, when their
                // canonical frame variables are available.
                continue;
            }
            if (const auto* floating = parameter_type
                    ? type_dyn_cast<Float>(parameter_type.get()) : nullptr) {
                const std::size_t width = floating->size_bytes();
                if (fp_index < 8 && (width == 4 || width == 8 || width == 16)) {
                    const std::string register_name =
                        abi_floating_register_name(
                            architecture, fp_index++, width);
                    signature_context.parameter_register_map[register_name]
                        = static_cast<int>(parameter_index);
                    task.set_parameter_register(register_name, DecompilerTask::ParameterInfo{
                        "a" + std::to_string(parameter_index + 1),
                        static_cast<int>(parameter_index),
                        parameter_type,
                    });
                }
            } else if (gp_index < architecture.gp_argument_registers.size()) {
                const std::string register_name =
                    architecture.gp_argument_registers[gp_index];
                ++gp_index;
                if (scalar_signature) {
                    const int index_value = static_cast<int>(parameter_index);
                    const std::string display = "a" + std::to_string(parameter_index + 1);
                    signature_context.parameter_register_map[register_name] = index_value;
                    task.set_parameter_register(register_name, DecompilerTask::ParameterInfo{
                        display, index_value, parameter_type});
                    if (architecture.arch_name == "arm64"
                        && register_name.starts_with('x')) {
                        std::string narrow_name = register_name;
                        narrow_name.front() = 'w';
                        signature_context.parameter_register_map[narrow_name]
                            = index_value;
                        task.set_parameter_register(narrow_name,
                            DecompilerTask::ParameterInfo{
                                display, index_value, parameter_type});
                    }
                }
            }
        }
    }
    PcodeLowerer lowerer(
        arena_,
        task,
        architecture,
        std::move(signature_context),
        PcodeLowerer::Options{collected_res->function_start, task.function_type()});

    if (flowchart_res->size() != cfg->blocks().size()) {
        return std::unexpected(ida::Error::internal(
            "P-Code CFG skeleton does not match IDA flowchart",
            std::to_string(cfg->blocks().size()) + " != " + std::to_string(flowchart_res->size())));
    }

    std::vector<std::optional<PcodeLowerer::CallArgumentEvidence>> block_evidence(
        flowchart_res->size());
    for (std::size_t index = 0; index < flowchart_res->size(); ++index) {
        BasicBlock* block = cfg->blocks()[index];
        const auto& ida_block = (*flowchart_res)[index];
        std::vector<PcodeLowerer::CallArgumentEvidence> predecessor_evidence;
        predecessor_evidence.reserve(ida_block.predecessors.size());
        for (const int predecessor_index : ida_block.predecessors) {
            if (predecessor_index < 0
                || static_cast<std::size_t>(predecessor_index) >= block_evidence.size()) {
                continue;
            }
            const auto& evidence = block_evidence[static_cast<std::size_t>(predecessor_index)];
            if (evidence.has_value()) {
                predecessor_evidence.push_back(*evidence);
            }
        }
        if (predecessor_evidence.size() != ida_block.predecessors.size()) {
            // A backedge or non-topological predecessor has no completed
            // snapshot yet; do not infer a partial reaching state.
            predecessor_evidence.clear();
        }

        lowerer.begin_basic_block(predecessor_evidence);
        for (ida::Address addr = ida_block.start; addr < ida_block.end; ) {
            auto raw_it = collected_res->instructions.find(addr);
            if (raw_it == collected_res->instructions.end()
                || raw_it->second.decoded_length == 0) {
                return std::unexpected(ida::Error::internal(
                    "Collected P-Code is missing an instruction",
                    "address=0x" + std::to_string(addr)));
            }
            auto lowered_res = lowerer.lower_instruction(raw_it->second);
            if (!lowered_res) {
                return std::unexpected(lowered_res.error());
            }
            for (Instruction* inst : *lowered_res) {
                block->add_instruction(inst);
            }
            addr += static_cast<ida::Address>(raw_it->second.decoded_length);
        }
        block_evidence[index] = lowerer.call_argument_evidence();
    }

    auto edge_status = connect_cfg_edges(
        arena_, task, *flowchart_res, *collected_res, *cfg);
    if (!edge_status) {
        return std::unexpected(edge_status.error());
    }

    std::size_t observed_parameter_count = 0;
    for (const auto& [storage, parameter] : task.parameter_registers()) {
        (void)storage;
        if (parameter.index >= 0) {
            observed_parameter_count = std::max(
                observed_parameter_count,
                static_cast<std::size_t>(parameter.index) + 1);
        }
    }
    if (const auto* recovered = task.function_type()
            ? type_dyn_cast<FunctionTypeDef>(task.function_type().get())
            : nullptr;
        recovered
        && recovered->parameters().size() < observed_parameter_count) {
        // IDA frequently materializes an auto-guessed zero-argument type for
        // unprototyped functions. Incoming ABI-register reads are stronger
        // evidence; let body refinement rebuild the signature transactionally.
        task.set_function_type(nullptr);
    }

    if (!task.function_type()) {
        bool removed_scalar_return = false;
        for (BasicBlock* block : cfg->blocks()) {
            auto& instructions = block->mutable_instructions();
            for (std::size_t return_index = 0;
                 return_index < instructions.size();
                 ++return_index) {
                auto* returned = dyn_cast<Return>(instructions[return_index]);
                if (!returned || returned->values().size() != 1) continue;
                auto* return_variable = dyn_cast<Variable>(returned->values()[0]);
                if (!return_variable
                    || architecture.integer_return_registers.empty()
                    || return_variable->name()
                        != architecture.integer_return_registers.front()) {
                    continue;
                }

                for (std::size_t cursor = return_index; cursor-- > 0; ) {
                    auto* assignment = dyn_cast<Assignment>(instructions[cursor]);
                    if (!assignment) continue;
                    if (auto* call = dyn_cast<Call>(assignment->value())) {
                        auto* call_type = call->ir_type()
                            ? type_dyn_cast<FunctionTypeDef>(call->ir_type().get())
                            : nullptr;
                        const bool call_returns_void = call_type
                            && call_type->return_type()
                            && call_type->return_type()->to_string() == "void";
                        if (call_returns_void) {
                            returned->mutable_values().clear();
                            removed_scalar_return = true;
                        }
                        break;
                    }
                    Expression* destination = assignment->destination();
                    while (auto* cast = dyn_cast<Operation>(destination)) {
                        if (cast->type() != OperationType::cast
                            || cast->operands().size() != 1) {
                            break;
                        }
                        destination = cast->operands()[0];
                    }
                    auto* variable = dyn_cast<Variable>(destination);
                    if (variable
                        && variable->name()
                            == architecture.integer_return_registers.front()) {
                        break;
                    }
                }
            }
        }
        if (removed_scalar_return) {
            bool all_returns_void = true;
            for (BasicBlock* block : cfg->blocks()) {
                for (Instruction* instruction : block->instructions()) {
                    auto* returned = dyn_cast<Return>(instruction);
                    if (returned && !returned->values().empty()) {
                        all_returns_void = false;
                        break;
                    }
                }
                if (!all_returns_void) {
                    break;
                }
            }
            if (all_returns_void) {
                task.set_function_type(std::make_shared<const FunctionTypeDef>(
                    CustomType::void_type(), std::vector<TypePtr>{}));
                task.add_frontend_diagnostic(FrontendDiagnostic{
                    FrontendDiagnosticSeverity::Info,
                    "pcode-inferred-void-return",
                    "Untyped function returns immediately after a void call without a later "
                    "integer ABI return-register definition",
                    task.function_address(),
                    0,
                });
            }
        }
    }

    refine_pcode_function_signature(
        task,
        *cfg,
        inferred_indirect_result_size,
        inferred_hfa_result.element_size,
        inferred_hfa_result.element_count,
        &architecture);

    return cfg;
#endif
}

} // namespace aletheia
