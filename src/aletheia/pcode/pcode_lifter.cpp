#include "pcode_lifter.hpp"

#include <ida/address.hpp>
#include <ida/database.hpp>
#include <ida/function.hpp>
#include <ida/graph.hpp>
#include <ida/instruction.hpp>
#include <ida/name.hpp>
#include <ida/type.hpp>

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
    std::unordered_map<ida::Address, RawPcodeInstruction> instructions;
    std::unordered_map<RegisterKey, std::string, RegisterKeyHash> register_names;
    std::string register_space_name = "register";
    std::string const_space_name = "const";
    std::string default_code_space_name = "code";
    std::string default_data_space_name = "ram";
};

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

std::optional<std::filesystem::path> find_arm64_spec_with_pspec() {
    auto sla_path = sleigh::FindSpecFile("AARCH64.sla", spec_search_paths());
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

ida::Result<std::filesystem::path> choose_arm64_spec(const ProcessorContext& context) {
    if (frontend::detect_arch() != "arm64" || context.bitness != 64 || context.big_endian) {
        return std::unexpected(ida::Error::unsupported(
            "P-Code MVP only supports little-endian ARM64",
            context.processor_name + " bits=" + std::to_string(context.bitness)));
    }

    if (auto path = find_arm64_spec_with_pspec(); path.has_value()) {
        return *path;
    }

    return std::unexpected(ida::Error::not_found(
        "Sleigh specification pair not found",
        "AARCH64.sla and adjacent AARCH64.pspec are required"));
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
    IdaDatabaseLoadImage()
        : ghidra::LoadImage("aletheia-pcode") {}

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

    std::string getArchType() const override { return "arm64"; }
    void adjustVma(long) override {}
};

struct SleighSession {
    explicit SleighSession(const std::filesystem::path& sla_path)
        : engine(&load_image, &context) {
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

    auto sla_path = choose_arm64_spec(*processor_context);
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
        SleighSession session(*sla_path);
        ghidra::Sleigh& sleigh_engine = session.engine;

        CollectedFunctionPcode collected;
        collected.function_start = function_start;
        collected.const_space_name = sleigh_engine.getConstantSpace()->getName();
        collected.default_code_space_name = sleigh_engine.getDefaultCodeSpace()->getName();
        if (auto* data_space = sleigh_engine.getDefaultDataSpace()) {
            collected.default_data_space_name = data_space->getName();
        }

        try {
            collected.register_space_name = sleigh_engine.getRegister("x0").space->getName();
        } catch (const ghidra::LowlevelError&) {
            return std::unexpected(ida::Error::unsupported(
                "Sleigh register model is incompatible",
                "AArch64 x0 register is unavailable"));
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
                    if (decoded_length != 4
                        || address + static_cast<ida::Address>(decoded_length) > block.end) {
                        return std::unexpected(ida::Error::unsupported(
                            "Unexpected AArch64 instruction length",
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
    frontend::FrameLayout frame_layout) {
    PcodeArchitectureContext context;
    context.arch_name = "arm64";
    context.pointer_size = 8;
    context.big_endian = false;
    context.register_space_name = collected.register_space_name;
    context.const_space_name = collected.const_space_name;
    context.data_space_name = collected.default_data_space_name;
    context.code_space_name = collected.default_code_space_name;

    const auto register_names = collected.register_names;
    context.register_mapper = [register_names](std::uint64_t offset, std::size_t size) -> std::optional<PcodeRegisterView> {
        RegisterKey exact{offset, size};
        auto it = register_names.find(exact);
        if (it == register_names.end()) {
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

    context.symbol_resolver = [](ida::Address address) -> std::optional<std::string> {
        if (auto name = ida::name::get(address); name && !name->empty()) {
            return frontend::sanitize_identifier(*name);
        }
        if (auto name = ida::function::name_at(address); name && !name->empty()) {
            return frontend::sanitize_identifier(*name);
        }
        return std::nullopt;
    };

    context.function_resolver = [](ida::Address address) -> std::optional<PcodeFunctionInfo> {
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
            recovered_type = true;
            info.prototype_known = true;
        }

        const std::string canonical = frontend::canonical_function_name(info.name);
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

        std::string name;
        if (auto resolved = frontend::resolve_frame_variable(frame_layout, offset, width)) {
            name = *resolved;
        } else if (offset < 0) {
            name = "local_" + std::to_string(static_cast<std::uint64_t>(-(offset + 1)) + 1);
        } else {
            name = "arg_" + std::to_string(static_cast<std::uint64_t>(offset));
        }

        auto* variable = arena.create<Variable>(name, width);
        variable->set_aliased(true);
        variable->set_stack_offset(offset);
        variable->set_kind(offset >= 0 ? VariableKind::StackArgument : VariableKind::StackLocal);
        auto exact = frame_layout.offset_to_var.find(offset);
        if (exact != frame_layout.offset_to_var.end() && exact->second.byte_size > 0) {
            variable->set_ir_type(std::make_shared<Integer>(exact->second.byte_size * 8, false));
        } else {
            variable->set_ir_type(std::make_shared<Integer>(width * 8, false));
        }
        return variable;
    };
    return context;
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
    return find_arm64_spec_with_pspec().has_value();
#else
    return false;
#endif
}

std::string pcode_runtime_unavailable_reason() {
#if ALETHEIA_HAS_PCODE
    if (!pcode_runtime_available()) {
        return "AARCH64.sla with an adjacent AARCH64.pspec was not found; set ALETHEIA_SLEIGH_SPEC_ROOT or build with ALETHEIA_PCODE_BUILD_SPECS=ON";
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
    auto frame_layout = frontend::build_frame_layout(collected_res->function_start);
    auto architecture = build_architecture_context(*collected_res, std::move(frame_layout));
    PcodeSignatureContext signature_context;
    signature_context.parameter_register_map = signature_state.param_register_map;
    signature_context.parameter_count_hint = signature_state.current_function_param_count_hint;
    signature_context.prefers_w_arguments = signature_state.current_function_prefers_w_args;
    signature_context.prefers_w_return = signature_state.current_function_prefers_w_return;

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
            if (const auto* floating = parameter_type
                    ? type_dyn_cast<Float>(parameter_type.get()) : nullptr) {
                const std::size_t width = floating->size_bytes();
                if (fp_index < 8 && (width == 4 || width == 8 || width == 16)) {
                    const char prefix = width == 4 ? 's' : (width == 8 ? 'd' : 'q');
                    const std::string register_name = std::string(1, prefix)
                        + std::to_string(fp_index++);
                    signature_context.parameter_register_map[register_name]
                        = static_cast<int>(parameter_index);
                    task.set_parameter_register(register_name, DecompilerTask::ParameterInfo{
                        "a" + std::to_string(parameter_index + 1),
                        static_cast<int>(parameter_index),
                        parameter_type,
                    });
                }
            } else if (gp_index < 8) {
                const std::string x_name = "x" + std::to_string(gp_index);
                const std::string w_name = "w" + std::to_string(gp_index);
                ++gp_index;
                if (scalar_signature) {
                    const int index_value = static_cast<int>(parameter_index);
                    const std::string display = "a" + std::to_string(parameter_index + 1);
                    signature_context.parameter_register_map[x_name] = index_value;
                    signature_context.parameter_register_map[w_name] = index_value;
                    task.set_parameter_register(x_name, DecompilerTask::ParameterInfo{
                        display, index_value, parameter_type});
                    task.set_parameter_register(w_name, DecompilerTask::ParameterInfo{
                        display, index_value, parameter_type});
                }
            }
        }
    }
    PcodeLowerer lowerer(
        arena_,
        task,
        std::move(architecture),
        std::move(signature_context),
        PcodeLowerer::Options{collected_res->function_start, task.function_type()});

    if (flowchart_res->size() != cfg->blocks().size()) {
        return std::unexpected(ida::Error::internal(
            "P-Code CFG skeleton does not match IDA flowchart",
            std::to_string(cfg->blocks().size()) + " != " + std::to_string(flowchart_res->size())));
    }

    for (std::size_t index = 0; index < flowchart_res->size(); ++index) {
        BasicBlock* block = cfg->blocks()[index];
        const auto& ida_block = (*flowchart_res)[index];
        lowerer.begin_basic_block();
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
    }

    auto edge_status = connect_cfg_edges(
        arena_, task, *flowchart_res, *collected_res, *cfg);
    if (!edge_status) {
        return std::unexpected(edge_status.error());
    }

    return cfg;
#endif
}

} // namespace aletheia
