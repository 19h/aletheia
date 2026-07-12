#include <ida/database.hpp>
#include <ida/data.hpp>
#include <ida/function.hpp>
#include <ida/fixup.hpp>
#include <ida/segment.hpp>

#include "../aletheia/pipeline/pipeline.hpp"
#include "../aletheia/frontend/frontend.hpp"
#include "../aletheia/frontend/shared_support.hpp"
#include "../aletheia/codegen/codegen.hpp"
#include "../aletheia/codegen/local_declarations.hpp"
#include "../aletheia/codegen/portable_c.hpp"
#include "../aletheia/lifter.hpp"
#include "../aletheia/ssa/ssa_constructor.hpp"
#include "../aletheia/ssa/ssa_destructor.hpp"
#include "../aletheia/pipeline/preprocessing_stages.hpp"
#include "../aletheia/pipeline/optimization_stages.hpp"
#include "../aletheia/pipeline/expressions/graph_expression_folding.hpp"
#include "../aletheia/pipeline/dataflow_analysis/dead_code_elimination.hpp"
#include "../aletheia/structuring/structuring_stage.hpp"
#include "../aletheia/structuring/instruction_length_handler.hpp"
#include "../aletheia/structuring/variable_name_generation.hpp"
#include "../aletheia/structuring/loop_name_generator.hpp"

#include "../aletheia/debug/debug_observer.hpp"
#include "../aletheia/debug/ir_serializer.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct CliOptions {
    std::string input_binary;
    std::string output_path;
    aletheia::FrontendKind frontend = aletheia::FrontendKind::Native;
    bool portable_c = false;
    bool explicit_headless = false;
    bool trace_pass_pseudocode = false;
    // Debug flags
    bool stage_metrics = false;
    bool stage_metrics_json = false;
    bool diff_stages = false;
    std::string diff_stage_name;
    bool check_invariants = false;
    std::string check_invariants_after;
    std::string trace_variable;
    bool dump_ir = false;
    bool debug_all = false;
};

bool env_flag_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    std::string_view v{value};
    return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
}

bool env_var_present(const char* name) {
    return std::getenv(name) != nullptr;
}

std::size_t env_size_t_or_default(const char* name, std::size_t fallback) {
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0') {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (!end || end == raw || *end != '\0') {
        return fallback;
    }
    return static_cast<std::size_t>(parsed);
}

bool should_enable_structuring() {
    if (env_flag_enabled("ALETHEIA_IDUMP_DISABLE_STRUCTURING")) {
        return false;
    }
    if (env_var_present("ALETHEIA_IDUMP_ENABLE_STRUCTURING")) {
        return env_flag_enabled("ALETHEIA_IDUMP_ENABLE_STRUCTURING");
    }
    return true;
}

bool ast_has_executable_content(aletheia::AstNode* node) {
    if (!node) {
        return false;
    }

    if (ast_dyn_cast<aletheia::ExprAstNode>(node) != nullptr) {
        return false;
    }

    if (auto* code = ast_dyn_cast<aletheia::CodeNode>(node)) {
        if (!code->block()) {
            return false;
        }
        for (aletheia::Instruction* inst : code->block()->instructions()) {
             if (!aletheia::isa<aletheia::Branch>(inst)
                && !aletheia::isa<aletheia::IndirectBranch>(inst)) {
                return true;
            }
        }
        return false;
    }

    if (auto* seq = ast_dyn_cast<aletheia::SeqNode>(node)) {
        for (aletheia::AstNode* child : seq->nodes()) {
            if (ast_has_executable_content(child)) {
                return true;
            }
        }
        return false;
    }

    if (auto* if_node = ast_dyn_cast<aletheia::IfNode>(node)) {
        return if_node->condition_expr() != nullptr
            || ast_has_executable_content(if_node->true_branch())
            || ast_has_executable_content(if_node->false_branch());
    }

    if (auto* loop = ast_dyn_cast<aletheia::LoopNode>(node)) {
        return loop->condition() != nullptr || ast_has_executable_content(loop->body());
    }

    if (auto* sw = ast_dyn_cast<aletheia::SwitchNode>(node)) {
        for (aletheia::CaseNode* case_node : sw->cases()) {
            if (ast_has_executable_content(case_node)) {
                return true;
            }
        }
        return false;
    }

    if (auto* case_node = ast_dyn_cast<aletheia::CaseNode>(node)) {
        return ast_has_executable_content(case_node->body());
    }

    // Conservative default for unknown AST node subclasses.
    return true;
}

std::string_view trim_ascii(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n')) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r' || text[last - 1] == '\n')) {
        --last;
    }
    return text.substr(first, last - first);
}

std::size_t count_emitted_executable_lines(const std::vector<std::string>& lines) {
    std::size_t count = 0;
    for (const std::string& line : lines) {
        std::string_view view = trim_ascii(line);
        if (view.empty()) {
            continue;
        }
        if (view == "{" || view == "}" || view == "} else {") {
            continue;
        }
        if (view.starts_with("/*")) {
            continue;
        }
        if (view.ends_with(':')) {
            continue;
        }
        if (view.starts_with("if (") || view.starts_with("while (") || view.starts_with("for (")
            || view.starts_with("switch (") || view.starts_with("do {") || view.starts_with("else if (")) {
            ++count;
            continue;
        }
        if (view.find(';') != std::string_view::npos) {
            ++count;
        }
    }
    return count;
}

std::size_t count_unresolved_control_markers(const std::vector<std::string>& lines) {
    std::size_t unresolved = 0;
    for (const std::string& line : lines) {
        if (line.find("goto bb_") != std::string::npos || line.find("/* branch if") != std::string::npos) {
            ++unresolved;
        }
    }
    return unresolved;
}

std::size_t count_structured_control_lines(const std::vector<std::string>& lines) {
    std::size_t structured = 0;
    for (const std::string& line : lines) {
        std::string_view view = trim_ascii(line);
        if (view.starts_with("if (") || view.starts_with("else if (") || view.starts_with("while (")
            || view.starts_with("for (") || view.starts_with("switch (") || view.starts_with("do {")
            || view.starts_with("case ") || view.starts_with("default:")) {
            ++structured;
        }
    }
    return structured;
}

bool generated_output_too_lossy(std::size_t lifted_non_control_count, const std::vector<std::string>& lines) {
    if (lifted_non_control_count < 6) {
        return false;
    }

    const std::size_t emitted = count_emitted_executable_lines(lines);
    if (emitted == 0) {
        return true;
    }

    const std::size_t unresolved = count_unresolved_control_markers(lines);
    if (unresolved == 0) {
        return false;
    }

    // Aggressive structured-first defaults: preserve mostly-structured output
    // unless unresolved control flow is both dense and frequent.
    const std::size_t min_markers = env_size_t_or_default("ALETHEIA_IDUMP_LOSSY_MIN_MARKERS", 6);
    const std::size_t max_unresolved_pct = env_size_t_or_default("ALETHEIA_IDUMP_LOSSY_MAX_UNRESOLVED_PCT", 25);
    const std::size_t min_structured_lines = env_size_t_or_default("ALETHEIA_IDUMP_LOSSY_MIN_STRUCTURED_LINES", 2);
    const std::size_t structured = count_structured_control_lines(lines);

    if (unresolved < min_markers) {
        return false;
    }

    const bool dense_unresolved = unresolved * 100 >= max_unresolved_pct * emitted;
    const bool weak_structured_signal = structured < min_structured_lines;
    return dense_unresolved || weak_structured_signal;
}

std::size_t count_cfg_non_control_instructions(const aletheia::ControlFlowGraph* cfg) {
    if (!cfg) {
        return 0;
    }
    std::size_t count = 0;
    for (aletheia::BasicBlock* block : cfg->blocks()) {
        if (!block) {
            continue;
        }
        for (aletheia::Instruction* inst : block->instructions()) {
             if (!aletheia::isa<aletheia::Branch>(inst)
                && !aletheia::isa<aletheia::IndirectBranch>(inst)) {
                ++count;
            }
        }
    }
    return count;
}

struct JsonMetricsPerFunction {
    std::string function_name;
    std::uint64_t function_address = 0;
    std::vector<aletheia::debug::StageMetrics> stages;
};

struct PortableGlobalBinding {
    std::string name;
    ida::Address address = 0;
    std::size_t required_size = 1;
    ida::Address segment_start = 0;
    ida::Address segment_end = 0;
    std::string segment_name;
    ida::segment::Type segment_type = ida::segment::Type::Undefined;
    bool writable = false;
};

struct DecompileDebugOutput {
    std::vector<aletheia::debug::StageMetrics> stage_metrics;
    std::string summary;
    std::string provenance_trace;
    aletheia::FrontendSupportReport frontend_support;
    std::vector<aletheia::FrontendDiagnostic> frontend_diagnostics;
    std::vector<PortableGlobalBinding> portable_globals;
};

struct RunScopedDebugState {
    std::unordered_set<std::string> emitted_unknown_selector_once;
};

using FunctionSignatureRegistry =
    std::unordered_map<ida::Address, aletheia::TypePtr>;

void install_function_signature_registry(
    aletheia::DecompilerTask& task,
    const FunctionSignatureRegistry* signatures) {
    if (!signatures) return;
    task.set_function_type_resolver(
        [signatures](ida::Address address) -> aletheia::TypePtr {
            auto found = signatures->find(address);
            return found != signatures->end() ? found->second : nullptr;
        });
}

std::vector<PortableGlobalBinding> collect_portable_global_bindings(
    aletheia::DecompilerTask& task) {
    struct Observation {
        ida::Address address = 0;
        std::size_t required_size = 1;
        bool has_address = false;
    };
    std::unordered_map<std::string, Observation> observations;
    std::unordered_set<const aletheia::Expression*> active;

    const auto observe = [&](aletheia::GlobalVariable* global, std::size_t extent) {
        if (!global || !global->represents_address() || global->name().empty()
            || global->name().front() == '"') {
            return;
        }
        Observation& observation = observations[global->name()];
        observation.required_size = std::max(
            observation.required_size, std::max<std::size_t>(extent, 1));
        if (auto* address = aletheia::dyn_cast<aletheia::Constant>(
                global->initial_value())) {
            observation.address = static_cast<ida::Address>(address->value());
            observation.has_address = true;
        }
    };

    const auto address_base = [&](const auto& self, aletheia::Expression* expression)
        -> std::optional<std::pair<aletheia::GlobalVariable*, std::uint64_t>> {
        if (!expression) return std::nullopt;
        if (auto* global = aletheia::dyn_cast<aletheia::GlobalVariable>(expression);
            global && global->represents_address()) {
            return std::pair{global, std::uint64_t{0}};
        }
        auto* operation = aletheia::dyn_cast<aletheia::Operation>(expression);
        if (!operation) return std::nullopt;
        if (operation->type() == aletheia::OperationType::cast
            && operation->operands().size() == 1) {
            return self(self, operation->operands()[0]);
        }
        if (operation->type() != aletheia::OperationType::add
            || operation->operands().size() != 2) {
            return std::nullopt;
        }
        for (std::size_t base_index = 0; base_index < 2; ++base_index) {
            auto base = self(self, operation->operands()[base_index]);
            auto* offset = aletheia::dyn_cast<aletheia::Constant>(
                operation->operands()[1 - base_index]);
            if (base && offset
                && base->second <= std::numeric_limits<std::uint64_t>::max()
                    - offset->value()) {
                base->second += offset->value();
                return base;
            }
        }
        return std::nullopt;
    };

    const auto scan_expression = [&](const auto& self, aletheia::Expression* expression) -> void {
        if (!expression || !active.insert(expression).second) return;
        if (auto* global = aletheia::dyn_cast<aletheia::GlobalVariable>(expression)) {
            observe(global, 1);
        }
        if (auto* call = aletheia::dyn_cast<aletheia::Call>(expression)) {
            auto* target = aletheia::dyn_cast<aletheia::GlobalVariable>(call->target());
            const std::string target_name = target
                ? aletheia::frontend::canonical_function_name(target->name()) : std::string{};
            std::optional<std::size_t> size_index;
            if (target_name == "bzero" && call->arg_count() >= 2) size_index = 1;
            if (target_name == "memset" && call->arg_count() >= 3) size_index = 2;
            if (size_index.has_value()) {
                auto base = address_base(address_base, call->arg(0));
                auto* size = aletheia::dyn_cast<aletheia::Constant>(call->arg(*size_index));
                if (base && size
                    && size->value() <= std::numeric_limits<std::size_t>::max()
                    && base->second <= std::numeric_limits<std::size_t>::max() - size->value()) {
                    observe(base->first,
                        static_cast<std::size_t>(base->second + size->value()));
                }
            }
            self(self, call->target());
            for (std::size_t index = 0; index < call->arg_count(); ++index) {
                self(self, call->arg(index));
            }
        } else if (auto* operation = aletheia::dyn_cast<aletheia::Operation>(expression)) {
            if (operation->type() == aletheia::OperationType::deref
                && operation->operands().size() == 1) {
                if (auto base = address_base(address_base, operation->operands()[0])) {
                    const std::uint64_t width = std::max<std::size_t>(operation->size_bytes, 1);
                    if (base->second <= std::numeric_limits<std::size_t>::max() - width) {
                        observe(base->first, static_cast<std::size_t>(base->second + width));
                    }
                }
            }
            for (aletheia::Expression* operand : operation->operands()) {
                self(self, operand);
            }
        } else if (auto* list = aletheia::dyn_cast<aletheia::ListOperation>(expression)) {
            for (aletheia::Expression* operand : list->operands()) self(self, operand);
        }
        active.erase(expression);
    };

    if (task.cfg()) {
        for (aletheia::BasicBlock* block : task.cfg()->blocks()) {
            if (!block) continue;
            for (aletheia::Instruction* instruction : block->instructions()) {
                if (auto* assignment = aletheia::dyn_cast<aletheia::Assignment>(instruction)) {
                    scan_expression(scan_expression, assignment->destination());
                    scan_expression(scan_expression, assignment->value());
                } else if (auto* returned = aletheia::dyn_cast<aletheia::Return>(instruction)) {
                    for (aletheia::Expression* value : returned->values()) {
                        scan_expression(scan_expression, value);
                    }
                } else if (auto* branch = aletheia::dyn_cast<aletheia::Branch>(instruction)) {
                    scan_expression(scan_expression, branch->condition());
                }
            }
        }
    }

    std::vector<PortableGlobalBinding> bindings;
    for (const auto& [name, observation] : observations) {
        if (!observation.has_address) continue;
        auto segment = ida::segment::at(observation.address);
        if (!segment) continue;
        bindings.push_back(PortableGlobalBinding{
            name,
            observation.address,
            observation.required_size,
            segment->start(),
            segment->end(),
            segment->name(),
            segment->type(),
            segment->permissions().write,
        });
    }
    return bindings;
}

struct EmittedFunction {
    ida::Address address = 0;
    bool ok = false;
    std::string error_message;
    std::vector<std::string> lines;
    bool declaration_only = false;
};

bool is_external_linkage_thunk(const ida::function::Function& function) {
    if (!function.is_thunk()) {
        return false;
    }

    auto segment_result = ida::segment::at(function.start());
    if (!segment_result) {
        return false;
    }

    const ida::segment::Segment& segment = *segment_result;
    if (segment.type() == ida::segment::Type::External
        || segment.type() == ida::segment::Type::Import) {
        return true;
    }

    // Executable import-stub section names used by Mach-O and ELF linkers.
    // FUNC_THUNK is also required above, so an ordinary function located in a
    // similarly named user section is not reclassified by the name alone.
    static const std::unordered_set<std::string> external_stub_sections = {
        "__stubs",
        "__auth_stubs",
        "__symbol_stub",
        "__symbol_stub1",
        ".plt",
        ".plt.sec",
        ".plt.got",
        ".iplt",
    };
    return external_stub_sections.contains(segment.name());
}

std::optional<std::string> extract_function_prototype(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        if (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
            continue;
        }
        if (line.find('(') == std::string::npos || !line.ends_with('{')) {
            continue;
        }
        std::string prototype = line.substr(0, line.size() - 1);
        while (!prototype.empty() && std::isspace(static_cast<unsigned char>(prototype.back()))) {
            prototype.pop_back();
        }
        prototype += ";";
        return prototype;
    }
    return std::nullopt;
}

std::optional<std::string> portable_c_unresolved_marker(
    const std::vector<std::string>& lines) {
    static constexpr std::string_view markers[] = {
        "__aletheia_undefined_u64(",
        "__aletheia_expr_cycle",
        "__aletheia_expr_depth_limit",
        "__aletheia_expr_expansion_limit",
        "__aletheia_unknown_op(",
        "__aletheia_unhandled_op(",
        "__aletheia_unresolved_list(",
        "ALETHEIA_UNSUPPORTED_CUSTOM_TYPE_WIDTH_",
        "ALETHEIA_UNSUPPORTED_COMPLEX_TYPE_LAYOUT",
        "ALETHEIA_UNSUPPORTED_PORTABLE_TYPE",
        "__pcode_userop_",
        " = phi(",
        "/* indirect branch ",
    };
    for (const std::string& line : lines) {
        for (std::string_view marker : markers) {
            if (line.find(marker) != std::string::npos) {
                if (marker.starts_with("ALETHEIA_UNSUPPORTED_")) {
                    return line;
                }
                return std::string(marker);
            }
        }
    }
    return std::nullopt;
}

void emit_portable_c_preamble(
    std::ostream& out,
    const std::vector<EmittedFunction>& functions,
    const std::unordered_map<std::string, PortableGlobalBinding>& globals,
    std::unordered_set<std::string>* materialized_names) {
    out << aletheia::portable_c_runtime_preamble();

    struct Region {
        ida::Address start = 0;
        ida::Address end = 0;
        ida::Address segment_start = 0;
        std::vector<const PortableGlobalBinding*> bindings;
    };
    std::unordered_map<ida::Address, Region> regions_by_segment;
    const auto internal_materializable = [](const PortableGlobalBinding& binding) {
        if (binding.segment_type == ida::segment::Type::External
            || binding.segment_type == ida::segment::Type::Import) {
            return false;
        }
        std::string segment_name = binding.segment_name;
        std::transform(segment_name.begin(), segment_name.end(), segment_name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return segment_name.find("got") == std::string::npos
            && segment_name.find("symbol_ptr") == std::string::npos
            && segment_name.find("stub") == std::string::npos;
    };
    for (const auto& [_, binding] : globals) {
        if (!internal_materializable(binding)
            || binding.address >= binding.segment_end) continue;
        const std::size_t available = static_cast<std::size_t>(
            binding.segment_end - binding.address);
        // Preserve one aligned data lane around symbols used as the base of
        // compact constant tables. Optimized code commonly indexes 8-byte
        // elements from an IDA symbol whose nominal database extent is one
        // byte (for example a two-entry floating constant table).
        const std::size_t extent = std::min(
            std::max<std::size_t>(binding.required_size, 16), available);
        if (extent == 0) continue;
        Region& region = regions_by_segment[binding.segment_start];
        region.segment_start = binding.segment_start;
        if (region.bindings.empty()) {
            region.start = binding.address;
            region.end = binding.address + extent;
        } else {
            region.start = std::min(region.start, binding.address);
            region.end = std::max(region.end, binding.address + extent);
        }
        region.bindings.push_back(&binding);
    }

    std::vector<Region> regions;
    regions.reserve(regions_by_segment.size());
    for (auto& [_, region] : regions_by_segment) regions.push_back(std::move(region));
    std::sort(regions.begin(), regions.end(),
        [](const Region& lhs, const Region& rhs) { return lhs.start < rhs.start; });
    struct Relocation {
        std::size_t source_region = 0;
        std::size_t source_offset = 0;
        ida::Address source_address = 0;
        ida::Address target_address = 0;
    };
    std::vector<Relocation> relocations;
    for (std::size_t index = 0; index < regions.size(); ++index) {
        Region& region = regions[index];
        region.start = std::max(
            region.segment_start,
            static_cast<ida::Address>(region.start & ~ida::Address{0x0f}));
        std::sort(region.bindings.begin(), region.bindings.end(),
            [](const PortableGlobalBinding* lhs, const PortableGlobalBinding* rhs) {
                if (lhs->address != rhs->address) return lhs->address < rhs->address;
                return lhs->name < rhs->name;
            });
        const std::size_t size = static_cast<std::size_t>(region.end - region.start);
        std::vector<std::uint8_t> bytes(size, 0);
        if (auto read = ida::data::read_bytes(region.start, size);
            read && read->size() == size) {
            std::copy(read->begin(), read->end(), bytes.begin());
        }
        if (auto fixups = ida::fixup::in_range(region.start, region.end)) {
            for (const ida::fixup::Descriptor& fixup : *fixups) {
                if (fixup.type != ida::fixup::Type::Off64
                    || fixup.source < region.start
                    || fixup.source - region.start > size
                    || size - static_cast<std::size_t>(fixup.source - region.start)
                        < sizeof(std::uint64_t)) {
                    continue;
                }
                const std::size_t source_offset = static_cast<std::size_t>(
                    fixup.source - region.start);
                std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(source_offset),
                    sizeof(std::uint64_t), std::uint8_t{0});
                relocations.push_back(Relocation{
                    index,
                    source_offset,
                    fixup.source,
                    fixup.target,
                });
            }
        }
        out << "_Alignas(16) static unsigned char __aletheia_data_region_"
            << index << '[' << size << "] = {";
        std::size_t emitted_bytes = 0;
        for (std::size_t byte_index = 0; byte_index < bytes.size(); ++byte_index) {
            if (bytes[byte_index] == 0) continue;
            if (emitted_bytes % 4 == 0) out << "\n    ";
            static constexpr char digits[] = "0123456789abcdef";
            const std::uint8_t byte = bytes[byte_index];
            out << '[' << byte_index << "] = 0x"
                << digits[byte >> 4] << digits[byte & 0x0f] << ", ";
            ++emitted_bytes;
        }
        if (emitted_bytes == 0) out << "\n    0";
        out << "\n};\n";
        for (const PortableGlobalBinding* binding : region.bindings) {
            if (!binding || binding->address < region.start) continue;
            const std::uint64_t offset = binding->address - region.start;
            out << "#define " << binding->name << " (__aletheia_data_region_"
                << index << " + UINT64_C(" << offset << "))\n";
            if (materialized_names) materialized_names->insert(binding->name);
        }
        out << '\n';
    }

    if (!relocations.empty()) {
        out << "extern uintptr_t __aletheia_resolve_external_data("
               "uint64_t source_address, uint64_t target_address) "
               "__attribute__((weak));\n";
        out << "static void __aletheia_relocate_data(void) "
               "__attribute__((constructor));\n"
               "static void __aletheia_relocate_data(void) {\n";
        for (const Relocation& relocation : relocations) {
            std::optional<std::pair<std::size_t, std::size_t>> internal_target;
            for (std::size_t target_index = 0; target_index < regions.size(); ++target_index) {
                if (relocation.target_address >= regions[target_index].start
                    && relocation.target_address < regions[target_index].end) {
                    internal_target = std::pair{
                        target_index,
                        static_cast<std::size_t>(
                            relocation.target_address - regions[target_index].start)};
                    break;
                }
            }
            out << "    { uintptr_t value = ";
            if (internal_target) {
                out << "(uintptr_t)(__aletheia_data_region_"
                    << internal_target->first << " + " << internal_target->second << ");";
            } else {
                out << "__aletheia_resolve_external_data ? "
                    << "__aletheia_resolve_external_data(UINT64_C("
                    << relocation.source_address << "), UINT64_C("
                    << relocation.target_address << ")) : UINT64_C(0);";
            }
            out << " __builtin_memcpy(__aletheia_data_region_"
                << relocation.source_region << " + " << relocation.source_offset
                << ", &value, sizeof(value)); }\n";
        }
        out << "}\n\n";
    }

    for (const EmittedFunction& function : functions) {
        if (!function.ok) {
            continue;
        }
        if (auto prototype = extract_function_prototype(function.lines)) {
            out << *prototype << '\n';
        }
    }
    out << '\n';
}

bool is_materialized_global_declaration(
    std::string_view line,
    const std::unordered_set<std::string>& materialized_names) {
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos
        || !line.substr(first).starts_with("extern ")) {
        return false;
    }
    for (const std::string& name : materialized_names) {
        const std::string suffix = " " + name + "[];";
        if (line.ends_with(suffix)) return true;
    }
    return false;
}

std::string dedupe_selector_diagnostic_per_run(
    std::string summary,
    const aletheia::debug::DebugOptions& debug_opts,
    RunScopedDebugState* run_state) {
    if (!run_state || debug_opts.check_invariants_after.empty()) {
        return summary;
    }
    if (!summary.starts_with("debug: --check-invariants-after selector")) {
        return summary;
    }

    if (run_state->emitted_unknown_selector_once.contains(debug_opts.check_invariants_after)) {
        while (summary.starts_with("debug:")) {
            auto nl = summary.find('\n');
            if (nl == std::string::npos) {
                summary.clear();
                break;
            }
            summary.erase(0, nl + 1);
        }
        return summary;
    }

    run_state->emitted_unknown_selector_once.insert(debug_opts.check_invariants_after);
    return summary;
}

class NullBuffer final : public std::streambuf {
public:
    int overflow(int c) override { return c; }
};

class ScopedStdoutSilencer final {
public:
    explicit ScopedStdoutSilencer(bool enabled) {
        if (!enabled) {
            return;
        }
        backup_fd_ = ::dup(STDOUT_FILENO);
        if (backup_fd_ < 0) {
            return;
        }
        null_fd_ = ::open("/dev/null", O_WRONLY);
        if (null_fd_ < 0) {
            ::close(backup_fd_);
            backup_fd_ = -1;
            return;
        }
        if (::dup2(null_fd_, STDOUT_FILENO) < 0) {
            ::close(null_fd_);
            ::close(backup_fd_);
            null_fd_ = -1;
            backup_fd_ = -1;
            return;
        }
        active_ = true;
    }

    ~ScopedStdoutSilencer() { restore(); }

    ScopedStdoutSilencer(const ScopedStdoutSilencer&) = delete;
    ScopedStdoutSilencer& operator=(const ScopedStdoutSilencer&) = delete;

    void restore() {
        if (!active_) {
            return;
        }
        ::fflush(stdout);
        ::dup2(backup_fd_, STDOUT_FILENO);
        ::close(backup_fd_);
        ::close(null_fd_);
        backup_fd_ = -1;
        null_fd_ = -1;
        active_ = false;
    }

private:
    int backup_fd_ = -1;
    int null_fd_ = -1;
    bool active_ = false;
};

void print_usage() {
    std::cerr << "Usage: idump <binary> [-o <output.c>] [--frontend=native|pcode] [--emit=pseudocode|portable-c] [--headless] [--trace-pass-pseudocode]\n"
              << "  Debug options:\n"
              << "    --stage-metrics           Print per-stage timing and IR size table\n"
              << "    --stage-metrics=json      Print per-stage timing/size as JSON\n"
              << "    --diff-stages             Print per-stage IR diffs\n"
              << "    --diff-stage=NAME         Diff for a specific stage only\n"
              << "    --check-invariants        Run IR invariant checks after each stage\n"
              << "    --check-invariants-after=SELECTOR  Run checks only after stage (Name or Name#N)\n"
              << "    --trace-variable=NAME     Trace variable mutations through pipeline\n"
              << "    --dump-ir                 Dump full IR after each stage\n"
              << "    --debug-all               Enable all debug features\n";
}

bool parse_args(int argc, char** argv, CliOptions& options) {
    if (argc < 2) {
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") {
            options.explicit_headless = true;
            continue;
        }
        if (arg == "--trace-pass-pseudocode") {
            options.trace_pass_pseudocode = true;
            continue;
        }
        if (arg.starts_with("--frontend=")) {
            aletheia::FrontendKind parsed = aletheia::FrontendKind::Native;
            if (!aletheia::parse_frontend_kind(arg.substr(11), parsed)) {
                return false;
            }
            options.frontend = parsed;
            continue;
        }
        if (arg == "--emit=portable-c") {
            options.portable_c = true;
            continue;
        }
        if (arg == "--emit=pseudocode") {
            options.portable_c = false;
            continue;
        }
        if (arg == "-o") {
            if (i + 1 >= argc) return false;
            options.output_path = argv[++i];
            continue;
        }
        if (arg == "--stage-metrics") { options.stage_metrics = true; continue; }
        if (arg == "--stage-metrics=json") {
            options.stage_metrics = true;
            options.stage_metrics_json = true;
            continue;
        }
        if (arg == "--diff-stages") { options.diff_stages = true; continue; }
        if (arg.starts_with("--diff-stage=")) { options.diff_stage_name = arg.substr(13); options.diff_stages = true; continue; }
        if (arg == "--check-invariants") { options.check_invariants = true; continue; }
        if (arg.starts_with("--check-invariants-after=")) {
            options.check_invariants_after = arg.substr(25);
            continue;
        }
        if (arg.starts_with("--trace-variable=")) { options.trace_variable = arg.substr(17); continue; }
        if (arg == "--dump-ir") { options.dump_ir = true; continue; }
        if (arg == "--debug-all") { options.debug_all = true; continue; }
        if (!arg.empty() && arg[0] == '-') {
            return false;
        }
        if (options.input_binary.empty()) {
            options.input_binary = arg;
        } else {
            return false;
        }
    }

    return !options.input_binary.empty();
}

bool detect_headless(const CliOptions& options) {
    if (options.explicit_headless) return true;
    if (env_flag_enabled("ALETHEIA_HEADLESS")) return true;
    return false;
}

bool should_trace_pass_pseudocode(const CliOptions& options) {
    if (options.trace_pass_pseudocode) {
        return true;
    }
    if (env_flag_enabled("ALETHEIA_IDUMP_TRACE_PASS_PSEUDOCODE")) {
        return true;
    }
    if (env_flag_enabled("ALETHEIA_IDUMP_TRACE_PASSES")) {
        return true;
    }
    return false;
}

void configure_out_of_ssa_mode(aletheia::DecompilerTask& task) {
    if (const char* mode_env = std::getenv("ALETHEIA_OUT_OF_SSA_MODE"); mode_env != nullptr) {
        auto parsed = aletheia::SsaDestructor::parse_mode(mode_env);
        if (parsed.has_value()) {
            task.set_out_of_ssa_mode(*parsed);
        }
    }
}

void apply_variable_naming(aletheia::DecompilerTask& task) {
    if (const char* naming_env = std::getenv("ALETHEIA_VARIABLE_NAMING"); naming_env != nullptr) {
        std::string_view scheme{naming_env};
        if (scheme == "system_hungarian") {
            aletheia::VariableNameGeneration::apply_system_hungarian(task.ast());
        } else {
            aletheia::VariableNameGeneration::apply_default(task.ast());
        }
    } else {
        aletheia::VariableNameGeneration::apply_default(task.ast());
    }
    aletheia::LoopNameGenerator::apply_for_loop_counters(task.ast());
    aletheia::LoopNameGenerator::apply_while_loop_counters(task.ast());
    // Remove self-assignments that become visible after rename collapses
    // different SSA versions of the same register to the same name.
    aletheia::VariableNameGeneration::remove_self_assignments(task.cfg(), &task.arena());
    // Also remove from AST-wrapped blocks (may differ from flat CFG block list).
    aletheia::VariableNameGeneration::remove_self_assignments_ast(task.ast(), &task.arena());
}

std::string block_label(const aletheia::BasicBlock* block) {
    return "bb_" + std::to_string(block ? block->id() : 0);
}

std::pair<aletheia::Edge*, aletheia::Edge*> pick_true_false_edges(aletheia::BasicBlock* block) {
    aletheia::Edge* true_edge = nullptr;
    aletheia::Edge* false_edge = nullptr;
    if (!block) {
        return {nullptr, nullptr};
    }

    for (aletheia::Edge* edge : block->successors()) {
        if (!edge) {
            continue;
        }
        if (edge->type() == aletheia::EdgeType::True && true_edge == nullptr) {
            true_edge = edge;
        } else if (edge->type() == aletheia::EdgeType::False && false_edge == nullptr) {
            false_edge = edge;
        }
    }

    if (!true_edge && !block->successors().empty()) {
        true_edge = block->successors().front();
    }
    if (!false_edge) {
        for (aletheia::Edge* edge : block->successors()) {
            if (edge != true_edge) {
                false_edge = edge;
                break;
            }
        }
    }

    return {true_edge, false_edge};
}

std::string indent_of(int level) {
    return std::string(static_cast<std::size_t>(level) * 4, ' ');
}

void emit_inline_branch_snapshot(
    aletheia::BasicBlock* block,
    aletheia::CExpressionGenerator& expr_gen,
    std::vector<std::string>& lines,
    int indent_level,
    int depth,
    std::unordered_set<aletheia::BasicBlock*>& path,
    std::unordered_set<aletheia::BasicBlock*>& inlined_blocks) {
    if (!block) {
        lines.push_back(indent_of(indent_level) + "/* unknown target */");
        return;
    }
    if (depth > 8) {
        lines.push_back(indent_of(indent_level) + "/* ... -> " + block_label(block) + " */");
        return;
    }
    if (path.contains(block)) {
        lines.push_back(indent_of(indent_level) + "/* loop -> " + block_label(block) + " */");
        return;
    }

    path.insert(block);
    inlined_blocks.insert(block);

    const auto& insts = block->instructions();
    aletheia::Instruction* tail = insts.empty() ? nullptr : insts.back();
    const bool tail_is_branch = aletheia::isa<aletheia::Branch>(tail);
    const bool tail_is_indirect = aletheia::isa<aletheia::IndirectBranch>(tail);

    std::size_t limit = insts.size();
    if ((tail_is_branch || tail_is_indirect) && limit > 0) {
        --limit;
    }

    for (std::size_t j = 0; j < limit; ++j) {
        std::string stmt = expr_gen.generate(insts[j]);
        if (!stmt.empty()) {
            lines.push_back(indent_of(indent_level) + stmt + ";");
        }
    }

    if (auto* branch = aletheia::dyn_cast<aletheia::Branch>(tail)) {
        auto [true_edge, false_edge] = pick_true_false_edges(block);
        const std::string cond = expr_gen.generate(branch->condition());
        lines.push_back(indent_of(indent_level) + "if (" + cond + ") {");
        emit_inline_branch_snapshot(
            true_edge ? true_edge->target() : nullptr,
            expr_gen,
            lines,
            indent_level + 1,
            depth + 1,
            path,
            inlined_blocks);
        lines.push_back(indent_of(indent_level) + "} else {");
        emit_inline_branch_snapshot(
            false_edge ? false_edge->target() : nullptr,
            expr_gen,
            lines,
            indent_level + 1,
            depth + 1,
            path,
            inlined_blocks);
        lines.push_back(indent_of(indent_level) + "}");
    } else if (auto* indirect = aletheia::dyn_cast<aletheia::IndirectBranch>(tail)) {
        const bool constant_target = aletheia::dyn_cast<aletheia::Constant>(indirect->expression()) != nullptr;
        const bool single_successor = block->successors().size() == 1;
        if (!(constant_target && single_successor)) {
            lines.push_back(indent_of(indent_level) + "/* indirect branch " + expr_gen.generate(indirect->expression()) + " */");
        }
        for (aletheia::Edge* edge : block->successors()) {
            emit_inline_branch_snapshot(
                edge ? edge->target() : nullptr,
                expr_gen,
                lines,
                indent_level,
                depth + 1,
                path,
                inlined_blocks);
        }
    } else if (block->successors().size() == 1) {
        aletheia::BasicBlock* next = block->successors()[0] ? block->successors()[0]->target() : nullptr;
        if (next) {
            emit_inline_branch_snapshot(next, expr_gen, lines, indent_level, depth + 1, path, inlined_blocks);
        }
    }

    path.erase(block);
}

bool is_declared_void_function(const aletheia::DecompilerTask& task) {
    if (!task.function_type()) {
        return true;
    }

    if (auto* func_type = type_dyn_cast<aletheia::FunctionTypeDef>(task.function_type().get())) {
        return func_type->return_type() && func_type->return_type()->to_string() == "void";
    }

    return task.function_type()->to_string() == "void";
}

std::string infer_return_type_from_expression(aletheia::Expression* expr) {
    if (expr && expr->ir_type()) {
        return expr->ir_type()->to_string();
    }

    const std::size_t size = expr ? expr->size_bytes : 0;
    switch (size) {
        case 1: return "unsigned char";
        case 2: return "unsigned short";
        case 4: return "int";
        case 8: return "long";
        default: return "int";
    }
}

aletheia::Expression* find_value_return_in_cfg(aletheia::ControlFlowGraph* cfg) {
    if (!cfg) return nullptr;

    for (aletheia::BasicBlock* block : cfg->blocks()) {
        if (!block) continue;
        for (aletheia::Instruction* inst : block->instructions()) {
            auto* ret = aletheia::dyn_cast<aletheia::Return>(inst);
            if (!ret || !ret->has_value() || ret->values().empty()) {
                continue;
            }
            if (ret->values()[0]) {
                return ret->values()[0];
            }
        }
    }

    return nullptr;
}

std::vector<std::string> generate_cfg_fallback_code(
    aletheia::DecompilerTask& task,
    bool portable_c = false) {
    std::vector<std::string> lines;
    if (auto validation =
            aletheia::validate_cfg_for_codegen(task)) {
        lines.push_back(
            "/* Aletheia CFG fallback rejected invalid IR: "
            + *validation + " */");
        return lines;
    }
    aletheia::CExpressionGenerator expr_gen({.portable_c = portable_c});

    // Set up parameter display name mapping.
    // Include both raw register names AND post-rename "arg_N" names.
    const aletheia::CIdentifierDisplayInfo identifier_display =
        aletheia::build_c_identifier_display_info(task);
    const aletheia::ParameterDisplayInfo& param_display =
        identifier_display.parameters;
    expr_gen.set_parameter_names(param_display.expr_name_map);
    expr_gen.set_parameter_index_names(param_display.index_to_name);
    expr_gen.set_parameter_types(param_display.declared_type_by_name);
    expr_gen.set_local_identifier_names(identifier_display.local_names);
    expr_gen.set_global_identifier_names(identifier_display.global_names);
    if (identifier_display.unresolved_type_semantics) {
        expr_gen.mark_unresolved_semantics();
    }

    if (portable_c && !identifier_display.type_declarations.empty()) {
        lines.insert(
            lines.end(),
            identifier_display.type_declarations.begin(),
            identifier_display.type_declarations.end());
        lines.push_back("");
    }

    auto global_decls = aletheia::GlobalDeclarationGenerator::generate(
        task, &identifier_display.global_names, portable_c);
    for (const auto& decl : global_decls) {
        lines.push_back(decl);
    }
    if (!global_decls.empty()) {
        lines.push_back("");
    }

    std::string return_type = "void";
    if (task.function_type()) {
        if (auto* func_type = type_dyn_cast<aletheia::FunctionTypeDef>(task.function_type().get())) {
            return_type = aletheia::render_codegen_type(
                func_type->return_type(), portable_c);
        } else {
            return_type = aletheia::render_codegen_type(
                task.function_type(), portable_c);
        }
    }

    if (is_declared_void_function(task)) {
        if (aletheia::Expression* return_value = find_value_return_in_cfg(task.cfg())) {
            return_type = infer_return_type_from_expression(return_value);
        }
    }

    std::string sig = return_type + " ";

    sig += identifier_display.function_name + "(";
    std::unordered_set<std::string> emitted_parameter_names;

    if (task.function_type()) {
        if (auto* func_type = type_dyn_cast<aletheia::FunctionTypeDef>(task.function_type().get())) {
            const auto& params = func_type->parameters();
            for (std::size_t i = 0; i < params.size(); ++i) {
                if (i > 0) sig += ", ";
                auto it = param_display.index_to_name.find(static_cast<int>(i));
                std::string pname = (it != param_display.index_to_name.end() && !it->second.empty())
                    ? it->second
                    : "a" + std::to_string(i + 1);
                sig += aletheia::render_codegen_declaration(
                    params[i], pname, portable_c);
                emitted_parameter_names.insert(pname);
            }
            if (func_type->variadic() && !params.empty()) {
                sig += ", ...";
            }
        }
    }
    if (emitted_parameter_names.empty() && !param_display.index_to_name.empty()) {
        std::vector<std::pair<int, std::string>> inferred_parameters(
            param_display.index_to_name.begin(), param_display.index_to_name.end());
        std::sort(inferred_parameters.begin(), inferred_parameters.end());
        bool first_parameter = true;
        for (const auto& [index, recovered_name] : inferred_parameters) {
            if (index < 0 || index > 31) continue;
            const std::string pname = recovered_name.empty()
                ? "arg_" + std::to_string(index) : recovered_name;
            if (!first_parameter) sig += ", ";
            sig += "unsigned long " + pname;
            emitted_parameter_names.insert(pname);
            first_parameter = false;
        }
    }
    sig += ") {";
    lines.push_back(sig);

    auto decls = aletheia::LocalDeclarationGenerator::generate(
        task, expr_gen, portable_c, &emitted_parameter_names);
    for (const auto& decl : decls) {
        lines.push_back("    " + decl);
    }
    if (!decls.empty()) {
        lines.push_back("");
    }

    if (task.cfg()) {
        const auto blocks = task.cfg()->blocks();
        if (portable_c) {
            for (aletheia::BasicBlock* block : blocks) {
                if (!block) continue;
                lines.push_back(block_label(block) + ":");
                const auto& instructions = block->instructions();
                aletheia::Instruction* tail = instructions.empty()
                    ? nullptr : instructions.back();
                const bool control_tail = aletheia::isa<aletheia::Branch>(tail)
                    || aletheia::isa<aletheia::IndirectBranch>(tail);
                const std::size_t limit = control_tail && !instructions.empty()
                    ? instructions.size() - 1 : instructions.size();
                for (std::size_t index = 0; index < limit; ++index) {
                    const std::string statement = expr_gen.generate(instructions[index]);
                    if (!statement.empty()) lines.push_back("    " + statement + ";");
                }

                if (auto* branch = aletheia::dyn_cast<aletheia::Branch>(tail)) {
                    auto [true_edge, false_edge] = pick_true_false_edges(block);
                    lines.push_back("    if (" + expr_gen.generate(branch->condition())
                        + ") goto " + block_label(true_edge ? true_edge->target() : nullptr)
                        + "; else goto " + block_label(false_edge ? false_edge->target() : nullptr)
                        + ";");
                } else if (aletheia::isa<aletheia::IndirectBranch>(tail)) {
                    if (block->successors().size() == 1 && block->successors()[0]) {
                        lines.push_back("    goto "
                            + block_label(block->successors()[0]->target()) + ";");
                    } else {
                        lines.push_back("    __builtin_trap();");
                    }
                } else if (!aletheia::isa<aletheia::Return>(tail)
                    && block->successors().size() == 1 && block->successors()[0]) {
                    lines.push_back("    goto "
                        + block_label(block->successors()[0]->target()) + ";");
                }
            }
            lines.push_back("}");
            return lines;
        }
        std::unordered_set<aletheia::BasicBlock*> inlined_blocks;
        aletheia::BasicBlock* entry = task.cfg()->entry_block();

        if (entry) {
            std::unordered_set<aletheia::BasicBlock*> path;
            emit_inline_branch_snapshot(entry, expr_gen, lines, 1, 0, path, inlined_blocks);
        }

        std::size_t omitted = 0;
        for (aletheia::BasicBlock* block : blocks) {
            if (block && !inlined_blocks.contains(block)) {
                ++omitted;
            }
        }
        if (omitted > 0) {
            lines.push_back("    /* detached blocks: " + std::to_string(omitted) + " */");
            lines.push_back("");

            for (aletheia::BasicBlock* block : blocks) {
                if (!block || inlined_blocks.contains(block)) {
                    continue;
                }

                lines.push_back("    /* detached " + block_label(block) + " */");

                const auto& insts = block->instructions();
                aletheia::Instruction* tail = insts.empty() ? nullptr : insts.back();
                const bool tail_is_branch = aletheia::isa<aletheia::Branch>(tail);
                const bool tail_is_indirect = aletheia::isa<aletheia::IndirectBranch>(tail);

                std::size_t limit = insts.size();
                if ((tail_is_branch || tail_is_indirect) && limit > 0) {
                    --limit;
                }

                for (std::size_t j = 0; j < limit; ++j) {
                    std::string stmt = expr_gen.generate(insts[j]);
                    if (!stmt.empty()) {
                        lines.push_back("        " + stmt + ";");
                    }
                }

                if (auto* branch = aletheia::dyn_cast<aletheia::Branch>(tail)) {
                    auto [true_edge, false_edge] = pick_true_false_edges(block);
                    const std::string cond = expr_gen.generate(branch->condition());
                    lines.push_back("        if (" + cond + ") {");
                    lines.push_back("            /* then -> " + block_label(true_edge ? true_edge->target() : nullptr) + " */");
                    lines.push_back("        } else {");
                    lines.push_back("            /* else -> " + block_label(false_edge ? false_edge->target() : nullptr) + " */");
                    lines.push_back("        }");
                } else if (auto* indirect = aletheia::dyn_cast<aletheia::IndirectBranch>(tail)) {
                    const bool constant_target = aletheia::dyn_cast<aletheia::Constant>(indirect->expression()) != nullptr;
                    const bool single_successor = block->successors().size() == 1;
                    if (!(constant_target && single_successor)) {
                        lines.push_back("        /* indirect branch " + expr_gen.generate(indirect->expression()) + " */");
                    }
                }

                lines.push_back("");
            }
        }
    }

    lines.push_back("}");
    return lines;
}

std::string join_snapshot_lines(const std::vector<std::string>& lines) {
    std::size_t total_size = 0;
    for (const auto& line : lines) {
        total_size += line.size() + 1;
    }

    std::string joined;
    joined.reserve(total_size);
    for (const auto& line : lines) {
        joined += line;
        joined.push_back('\n');
    }
    return joined;
}

class PassPseudocodeTracer {
public:
    PassPseudocodeTracer(ida::Address function_ea, bool enabled)
        : function_ea_(function_ea), enabled_(enabled) {}

    bool enabled() const { return enabled_; }

    void emit_stage_snapshot(
        const char* pipeline_name,
        const char* stage_name,
        bool before_stage,
        aletheia::DecompilerTask& task) {
        if (!enabled_) {
            return;
        }

        std::vector<std::string> lines = build_snapshot(task);
        std::string snapshot_text = join_snapshot_lines(lines);

        auto existing = snapshot_ids_.find(snapshot_text);
        if (existing != snapshot_ids_.end()) {
            std::cerr << "idump: " << pipeline_name << " " << (before_stage ? "before" : "after")
                      << " '" << stage_name << "' @0x" << std::hex << function_ea_ << std::dec
                      << " -> snapshot#" << existing->second << " (duplicate)\n";
            return;
        }

        const std::size_t snapshot_id = next_snapshot_id_++;
        snapshot_ids_.emplace(std::move(snapshot_text), snapshot_id);

        std::cerr << "idump: " << pipeline_name << " " << (before_stage ? "before" : "after")
                  << " '" << stage_name << "' @0x" << std::hex << function_ea_ << std::dec
                  << " -> snapshot#" << snapshot_id << "\n";
        if (lines.empty()) {
            std::cerr << "/* empty snapshot */\n\n";
            return;
        }
        for (const auto& line : lines) {
            std::cerr << line << '\n';
        }
        std::cerr << '\n';
    }

private:
    static std::vector<std::string> build_snapshot(aletheia::DecompilerTask& task) {
        if (task.ast() && task.ast()->root()) {
            aletheia::CodeVisitor visitor;
            return visitor.generate_code(task);
        }
        return generate_cfg_fallback_code(task);
    }

    ida::Address function_ea_;
    bool enabled_ = false;
    std::size_t next_snapshot_id_ = 1;
    std::unordered_map<std::string, std::size_t> snapshot_ids_;
};

aletheia::DecompilerPipeline build_pipeline(bool enable_structuring) {
    aletheia::DecompilerPipeline pipeline;
    pipeline.add_stage(std::make_unique<aletheia::CompilerIdiomHandlingStage>());
    pipeline.add_stage(std::make_unique<aletheia::RegisterPairHandlingStage>());
    pipeline.add_stage(std::make_unique<aletheia::RemoveGoPrologueStage>());
    pipeline.add_stage(std::make_unique<aletheia::RemoveStackCanaryStage>());
    pipeline.add_stage(std::make_unique<aletheia::RemoveNoreturnBoilerplateStage>());
    pipeline.add_stage(std::make_unique<aletheia::SwitchVariableDetectionStage>());
    pipeline.add_stage(std::make_unique<aletheia::CoherenceStage>());

    pipeline.add_stage(std::make_unique<aletheia::FallthroughBlockMergeStage>());
    pipeline.add_stage(std::make_unique<aletheia::LocalConstantFoldingStage>());
    pipeline.add_stage(std::make_unique<aletheia::InsertMissingDefinitionsStage>());
    pipeline.add_stage(std::make_unique<aletheia::MemPhiConverterStage>());
    pipeline.add_stage(std::make_unique<aletheia::SsaConstructor>());
    pipeline.add_stage(std::make_unique<aletheia::GraphExpressionFoldingStage>());
    pipeline.add_stage(std::make_unique<aletheia::DeadComponentPrunerStage>());
    pipeline.add_stage(std::make_unique<aletheia::ExpressionPropagationStage>());
    pipeline.add_stage(std::make_unique<aletheia::BitFieldComparisonUnrollingStage>());
    pipeline.add_stage(std::make_unique<aletheia::TypePropagationStage>());
    pipeline.add_stage(std::make_unique<aletheia::DeadPathEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::DeadLoopEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::ExpressionPropagationMemoryStage>());
    pipeline.add_stage(std::make_unique<aletheia::ExpressionPropagationFunctionCallStage>());
    pipeline.add_stage(std::make_unique<aletheia::DeadCodeEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::SinkDefinitionRepairStage>());
    pipeline.add_stage(std::make_unique<aletheia::VoidReturnNormalizationStage>());
    pipeline.add_stage(std::make_unique<aletheia::ReturnDefinitionSanityStage>());
    pipeline.add_stage(std::make_unique<aletheia::RedundantCastsEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::IdentityEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::CommonSubexpressionEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::AddressResolutionStage>());
    pipeline.add_stage(std::make_unique<aletheia::ArrayAccessDetectionStage>());
    pipeline.add_stage(std::make_unique<aletheia::ExpressionSimplificationStage>());
    pipeline.add_stage(std::make_unique<aletheia::DeadComponentPrunerStage>());
    pipeline.add_stage(std::make_unique<aletheia::GraphExpressionFoldingStage>());
    pipeline.add_stage(std::make_unique<aletheia::EdgePrunerStage>());
    pipeline.add_stage(std::make_unique<aletheia::PhiFunctionFixerStage>());
    pipeline.add_stage(std::make_unique<aletheia::SsaDestructor>());
    pipeline.add_stage(std::make_unique<aletheia::RedundantAssignmentEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::EmptyBasicBlockRemoverStage>());
    if (enable_structuring) {
        pipeline.add_stage(std::make_unique<aletheia::PatternIndependentRestructuringStage>());
        pipeline.add_stage(std::make_unique<aletheia::AstExpressionSimplificationStage>());
    }
    return pipeline;
}

std::optional<std::vector<std::string>> regenerate_conservative_fallback(
    ida::Address ea,
    idiomata::IdiomMatcher& matcher,
    aletheia::FrontendKind frontend_kind,
    bool portable_c,
    PassPseudocodeTracer* pass_tracer,
    aletheia::debug::DebugObserver* debug_observer,
    const FunctionSignatureRegistry* signatures) {
    aletheia::DecompilerTask fallback_task(ea);
    fallback_task.set_frontend_kind(frontend_kind);
    install_function_signature_registry(fallback_task, signatures);
    configure_out_of_ssa_mode(fallback_task);

    auto frontend_res = aletheia::create_frontend(frontend_kind, fallback_task.arena(), matcher);
    if (!frontend_res) {
        return std::nullopt;
    }
    std::vector<idiomata::IdiomTag> idiom_tags;

    (*frontend_res)->populate_task_signature(fallback_task);
    if (debug_observer) {
        (*debug_observer)("Lifter", true, fallback_task);
    }
    auto cfg_res = (*frontend_res)->lift_function(fallback_task, &idiom_tags);
    if (!cfg_res) {
        return std::nullopt;
    }

    fallback_task.set_cfg(std::move(*cfg_res));
    fallback_task.set_idiom_tags(std::move(idiom_tags));
    if (debug_observer) {
        (*debug_observer)("Lifter", false, fallback_task);
    }

    auto fallback_pipeline = build_pipeline(false);
    if ((pass_tracer && pass_tracer->enabled()) || debug_observer) {
        fallback_pipeline.run(
            fallback_task,
            [pass_tracer, debug_observer](const char* stage_name, bool before_stage, aletheia::DecompilerTask& observed_task) {
                if (pass_tracer && pass_tracer->enabled()) {
                    pass_tracer->emit_stage_snapshot("fallback-pass", stage_name, before_stage, observed_task);
                }
                if (debug_observer) {
                    (*debug_observer)(stage_name, before_stage, observed_task);
                }
            });
    } else {
        fallback_pipeline.run(fallback_task);
    }

    auto fallback_ast = std::make_unique<aletheia::AbstractSyntaxForest>();
    auto* seq = fallback_task.arena().create<aletheia::SeqNode>();
    if (fallback_task.cfg()) {
        for (aletheia::BasicBlock* block : fallback_task.cfg()->blocks()) {
            seq->add_node(fallback_task.arena().create<aletheia::CodeNode>(block));
        }
    }
    fallback_ast->set_root(seq);
    fallback_task.set_ast(std::move(fallback_ast));

    if (debug_observer) {
        (*debug_observer)("InstructionLengthHandler", true, fallback_task);
    }
    aletheia::InstructionLengthHandler::apply(fallback_task.ast(), fallback_task.arena());
    if (debug_observer) {
        (*debug_observer)("InstructionLengthHandler", false, fallback_task);
    }

    if (debug_observer) {
        (*debug_observer)("VariableNameGeneration", true, fallback_task);
    }
    apply_variable_naming(fallback_task);
    if (debug_observer) {
        (*debug_observer)("VariableNameGeneration", false, fallback_task);
    }

    if (aletheia::validate_task_for_codegen(fallback_task).has_value()) {
        return std::nullopt;
    }

    aletheia::CodeVisitor visitor({.portable_c = portable_c});
    std::vector<std::string> lines = visitor.generate_code(fallback_task);
    return lines;
}

struct SignatureDiscoveryResult {
    FunctionSignatureRegistry signatures;
    std::size_t passes = 0;
    bool converged = true;
};

SignatureDiscoveryResult discover_function_signatures(
    const std::vector<ida::Address>& function_addresses,
    idiomata::IdiomMatcher& matcher,
    aletheia::FrontendKind frontend_kind) {
    SignatureDiscoveryResult result;
    if (function_addresses.empty()) {
        return result;
    }
    std::vector<ida::Address> ordered_addresses = function_addresses;
    std::sort(ordered_addresses.begin(), ordered_addresses.end());
    ordered_addresses.erase(
        std::unique(ordered_addresses.begin(), ordered_addresses.end()),
        ordered_addresses.end());
    const bool x86_pcode_discovery =
        frontend_kind == aletheia::FrontendKind::Pcode
        && aletheia::frontend::detect_arch() == "x86_64";

    const auto registries_equal = [](const FunctionSignatureRegistry& lhs,
                                     const FunctionSignatureRegistry& rhs) {
        if (lhs.size() != rhs.size()) return false;
        for (const auto& [address, type] : lhs) {
            auto found = rhs.find(address);
            if (found == rhs.end() || !type || !found->second
                || *type != *found->second) {
                return false;
            }
        }
        return true;
    };

    const std::size_t maximum_passes = ordered_addresses.size() + 1;
    result.converged = false;
    for (std::size_t pass = 0; pass < maximum_passes; ++pass) {
        FunctionSignatureRegistry next = result.signatures;
        for (ida::Address address : ordered_addresses) {
            aletheia::DecompilerTask task(address);
            task.set_frontend_kind(frontend_kind);
            // Make signatures discovered earlier in this deterministic
            // address-ordered pass immediately available to their callers.
            // This prevents a provisional caller signature from becoming a
            // sticky fixed point merely because its callee was learned in the
            // same pass.
            install_function_signature_registry(
                task,
                x86_pcode_discovery ? &next : &result.signatures);
            auto frontend = aletheia::create_frontend(
                frontend_kind, task.arena(), matcher);
            if (!frontend) continue;
            (*frontend)->populate_task_signature(task);
            if (!task.function_type()
                && frontend_kind == aletheia::FrontendKind::Native) {
                std::size_t parameter_count = 0;
                for (const auto& [storage, parameter] :
                     task.parameter_registers()) {
                    (void)storage;
                    if (parameter.index >= 0) {
                        parameter_count = std::max(
                            parameter_count,
                            static_cast<std::size_t>(parameter.index) + 1);
                    }
                }
                if (parameter_count > 0) {
                    std::vector<aletheia::TypePtr> parameters(
                        parameter_count, aletheia::Integer::uint64_t());
                    task.set_function_type(
                        std::make_shared<const aletheia::FunctionTypeDef>(
                            aletheia::Integer::uint64_t(),
                            std::move(parameters)));
                }
            }
            if (frontend_kind == aletheia::FrontendKind::Pcode
                && (x86_pcode_discovery || !task.function_type())) {
                aletheia::TypePtr recovered_type = task.function_type();
                if (x86_pcode_discovery) {
                    // IDA auto-types for stripped x86-64 functions frequently
                    // collapse XMM lanes into integer/aggregate placeholders.
                    // Discovery must infer the current function from P-Code;
                    // recovered database types remain available as a fallback
                    // if the body cannot be lowered.
                    task.set_function_type(nullptr);
                }
                std::vector<idiomata::IdiomTag> idiom_tags;
                auto cfg = (*frontend)->lift_function(task, &idiom_tags);
                if (!cfg) {
                    if (!recovered_type) continue;
                    task.set_function_type(std::move(recovered_type));
                }
            }
            if (auto* function = task.function_type()
                    ? aletheia::type_dyn_cast<aletheia::FunctionTypeDef>(
                        task.function_type().get())
                    : nullptr) {
                (void)function;
                next[address] = task.function_type();
            }
        }
        ++result.passes;
        if (registries_equal(result.signatures, next)) {
            result.signatures = std::move(next);
            result.converged = true;
            break;
        }
        result.signatures = std::move(next);
    }
    return result;
}

std::vector<std::string> decompile_function(
    ida::Address ea,
    idiomata::IdiomMatcher& matcher,
    bool& ok,
    std::string& error_message,
    bool trace_pass_pseudocode,
    const CliOptions& cli_options = {},
    RunScopedDebugState* run_debug_state = nullptr,
    DecompileDebugOutput* debug_output = nullptr,
    const FunctionSignatureRegistry* signatures = nullptr) {
    ok = false;
    error_message.clear();

    aletheia::DecompilerTask task(ea);
    task.set_frontend_kind(cli_options.frontend);
    install_function_signature_registry(task, signatures);
    PassPseudocodeTracer pass_tracer(ea, trace_pass_pseudocode);
    configure_out_of_ssa_mode(task);

    if (debug_output) {
        debug_output->stage_metrics.clear();
        debug_output->summary.clear();
        debug_output->provenance_trace.clear();
        debug_output->frontend_support = {};
        debug_output->frontend_diagnostics.clear();
        debug_output->portable_globals.clear();
    }

    // Setup debug observer
    aletheia::debug::DebugOptions debug_opts;
    debug_opts.stage_metrics = cli_options.stage_metrics;
    debug_opts.stage_metrics_json = cli_options.stage_metrics_json;
    debug_opts.diff_stages = cli_options.diff_stages;
    debug_opts.diff_stage_name = cli_options.diff_stage_name;
    debug_opts.check_invariants = cli_options.check_invariants;
    debug_opts.check_invariants_after = cli_options.check_invariants_after;
    debug_opts.trace_variable = cli_options.trace_variable;
    debug_opts.dump_ir = cli_options.dump_ir;
    debug_opts.debug_all = cli_options.debug_all;
    const bool has_debug = debug_opts.stage_metrics || debug_opts.diff_stages ||
                           debug_opts.check_invariants || !debug_opts.check_invariants_after.empty() ||
                           !debug_opts.trace_variable.empty() ||
                           debug_opts.dump_ir || debug_opts.debug_all;

    NullBuffer null_buffer;
    std::ostream null_stream(&null_buffer);
    std::ostream& debug_stream = debug_opts.stage_metrics_json ? null_stream : std::cerr;
    std::optional<aletheia::debug::DebugObserver> debug_observer;
    if (has_debug) {
        debug_observer.emplace(debug_opts, debug_stream);
    }
    const bool emit_human_debug = has_debug && !debug_opts.stage_metrics_json;

    auto emit_debug_outputs = [&]() {
        if (debug_output) {
            debug_output->frontend_support = task.frontend_support_report();
            debug_output->frontend_diagnostics = task.frontend_diagnostics();
            debug_output->portable_globals = collect_portable_global_bindings(task);
        }
        if (!has_debug || !debug_observer.has_value()) {
            return;
        }
        std::string debug_summary = dedupe_selector_diagnostic_per_run(
            debug_observer->format_summary(),
            debug_opts,
            run_debug_state);
        std::string provenance_trace;
        if (!debug_opts.trace_variable.empty()) {
            provenance_trace = debug_observer->provenance().format_trace(debug_opts.trace_variable);
        }

        if (debug_output) {
            debug_output->stage_metrics = debug_observer->metrics().metrics();
            debug_output->summary = debug_summary;
            debug_output->provenance_trace = provenance_trace;
        }

        if (emit_human_debug) {
            if (!debug_summary.empty()) {
                std::cerr << debug_summary;
            }
            if (!provenance_trace.empty()) {
                std::cerr << provenance_trace;
            }
        }
    };

    auto frontend_res = aletheia::create_frontend(cli_options.frontend, task.arena(), matcher);
    if (!frontend_res) {
        error_message = frontend_res.error().message;
        if (!frontend_res.error().context.empty()) {
            error_message += ": " + frontend_res.error().context;
        }
        return {};
    }
    std::vector<idiomata::IdiomTag> idiom_tags;

    (*frontend_res)->populate_task_signature(task);

    // Capture pre-lifter state as synthetic stage boundary (baseline for metrics/provenance)
    if (has_debug) {
        (*debug_observer)("Lifter", true, task);
    }

    auto cfg_res = (*frontend_res)->lift_function(task, &idiom_tags);
    if (!cfg_res) {
        emit_debug_outputs();
        error_message = cfg_res.error().message;
        if (!cfg_res.error().context.empty()) {
            error_message += ": " + cfg_res.error().context;
        }
        return {};
    }

    const std::size_t lifted_non_control_count = count_cfg_non_control_instructions((*cfg_res).get());

    task.set_cfg(std::move(*cfg_res));
    task.set_idiom_tags(std::move(idiom_tags));

    std::size_t lifted_branch_count = 0;
    if (task.cfg()) {
        for (aletheia::BasicBlock* block : task.cfg()->blocks()) {
            if (!block) continue;
            for (aletheia::Instruction* instruction : block->instructions()) {
                if (aletheia::isa<aletheia::Branch>(instruction)) {
                    ++lifted_branch_count;
                }
            }
        }
    }

    // Capture lifter output as synthetic stage boundary
    if (has_debug) {
        (*debug_observer)("Lifter", false, task);
    }

    const bool enable_structuring = should_enable_structuring();
    const bool force_structured_output = env_flag_enabled("ALETHEIA_IDUMP_FORCE_STRUCTURED_OUTPUT");
    auto pipeline = build_pipeline(enable_structuring);
    if (has_debug || pass_tracer.enabled()) {
        pipeline.run(
            task,
            [&pass_tracer, &debug_observer, has_debug](const char* stage_name, bool before_stage, aletheia::DecompilerTask& observed_task) {
                if (pass_tracer.enabled()) {
                    pass_tracer.emit_stage_snapshot("pipeline-pass", stage_name, before_stage, observed_task);
                }
                if (has_debug) {
                    (*debug_observer)(stage_name, before_stage, observed_task);
                }
            });
    } else {
        pipeline.run(task);
    }

    if (task.failed() && !debug_opts.stage_metrics_json) {
        std::cerr << "idump: pipeline stopped at stage '" << task.failure_stage() << "'";
        if (!task.failure_message().empty()) {
            std::cerr << ": " << task.failure_message();
        }
        std::cerr << "\n";
    }

    const bool duplicate_ast_ownership = task.ast() && task.ast()->root()
        && !aletheia::ast_has_unique_code_node_ownership(task.ast()->root());
    bool using_cfg_fallback = !task.ast() || !task.ast()->root();
    if (!using_cfg_fallback && enable_structuring) {
        using_cfg_fallback = !ast_has_executable_content(task.ast()->root())
            || duplicate_ast_ownership;
    }

    if (using_cfg_fallback) {
        auto fallback_ast = std::make_unique<aletheia::AbstractSyntaxForest>();
        auto* seq = task.arena().create<aletheia::SeqNode>();
        if (task.cfg()) {
            for (aletheia::BasicBlock* block : task.cfg()->blocks()) {
                seq->add_node(task.arena().create<aletheia::CodeNode>(block));
            }
        }
        fallback_ast->set_root(seq);
        task.set_ast(std::move(fallback_ast));
    }

    if (using_cfg_fallback) {
        if (enable_structuring && !force_structured_output) {
            if (auto rebuilt = regenerate_conservative_fallback(
                    ea, matcher, cli_options.frontend, cli_options.portable_c,
                    &pass_tracer, has_debug ? &(*debug_observer) : nullptr,
                    signatures);
                rebuilt.has_value()) {
                emit_debug_outputs();
                ok = true;
                return *rebuilt;
            }
        }

        if (has_debug) {
            (*debug_observer)("InstructionLengthHandler", true, task);
        }
        aletheia::InstructionLengthHandler::apply(task.ast(), task.arena());
        if (has_debug) {
            (*debug_observer)("InstructionLengthHandler", false, task);
        }

        if (has_debug) {
            (*debug_observer)("VariableNameGeneration", true, task);
        }
        apply_variable_naming(task);
        if (has_debug) {
            (*debug_observer)("VariableNameGeneration", false, task);
        }
        aletheia::VariableNameGeneration::apply_to_cfg(task.cfg());

        emit_debug_outputs();

        if (auto validation =
                aletheia::validate_cfg_for_codegen(task)) {
            error_message =
                "CFG fallback rejected invalid IR: " + *validation;
            return {};
        }
        ok = true;
        return generate_cfg_fallback_code(task, cli_options.portable_c);
    }

    if (has_debug) {
        (*debug_observer)("InstructionLengthHandler", true, task);
    }
    aletheia::InstructionLengthHandler::apply(task.ast(), task.arena());
    if (has_debug) {
        (*debug_observer)("InstructionLengthHandler", false, task);
    }

    if (has_debug) {
        (*debug_observer)("VariableNameGeneration", true, task);
    }
    apply_variable_naming(task);
    if (has_debug) {
        (*debug_observer)("VariableNameGeneration", false, task);
    }

    emit_debug_outputs();

    aletheia::CodeVisitor visitor({.portable_c = cli_options.portable_c});
    auto structured_lines = visitor.generate_code(task);

    if (cli_options.portable_c && task.cfg()) {
        std::size_t cfg_predicates = 0;
        for (aletheia::BasicBlock* block : task.cfg()->blocks()) {
            if (!block) continue;
            for (aletheia::Instruction* instruction : block->instructions()) {
                if (aletheia::isa<aletheia::Branch>(instruction)) {
                    ++cfg_predicates;
                }
            }
        }
        cfg_predicates = std::max(cfg_predicates, lifted_branch_count);
        std::size_t emitted_predicates = 0;
        for (const std::string& line : structured_lines) {
            const std::size_t first = line.find_first_not_of(" \t");
            const std::string_view trimmed = first == std::string::npos
                ? std::string_view{} : std::string_view(line).substr(first);
            if (trimmed.starts_with("if (") || trimmed.starts_with("while (")
                || trimmed.starts_with("for (")) {
                ++emitted_predicates;
            }
        }
        if (emitted_predicates < cfg_predicates) {
            if (auto rebuilt = regenerate_conservative_fallback(
                    ea, matcher, cli_options.frontend, cli_options.portable_c,
                    &pass_tracer, has_debug ? &(*debug_observer) : nullptr,
                    signatures);
                rebuilt.has_value()) {
                emit_debug_outputs();
                ok = true;
                return *rebuilt;
            }
            if (auto validation =
                    aletheia::validate_cfg_for_codegen(task)) {
                error_message =
                    "CFG fallback rejected invalid IR: " + *validation;
                return {};
            }
            ok = true;
            return generate_cfg_fallback_code(task, true);
        }
    }

    if (enable_structuring && !force_structured_output
        && generated_output_too_lossy(lifted_non_control_count, structured_lines)) {
        if (ea == 0 && !debug_opts.stage_metrics_json) {
            std::cerr << "STRUCTURED LINES FOR FUNC 0:" << std::endl;
            for (const auto& l : structured_lines) {
                std::cerr << "  " << l << std::endl;
            }
        }
        if (!debug_opts.stage_metrics_json) {
            std::cerr << "FALLING BACK FOR FUNCTION " << std::hex << ea << std::dec << std::endl;
        }
        if (auto rebuilt = regenerate_conservative_fallback(
                ea, matcher, cli_options.frontend, cli_options.portable_c,
                &pass_tracer, has_debug ? &(*debug_observer) : nullptr,
                signatures);
            rebuilt.has_value()) {
            emit_debug_outputs();
            ok = true;
            return *rebuilt;
        }
    }

    if (cli_options.portable_c && visitor.has_unresolved_semantics()) {
        error_message = "portable-C code generation encountered unresolved semantics";
        return {};
    }

    ok = true;
    return structured_lines;
}

} // namespace

int main(int argc, char** argv) {
    CliOptions options;
    if (!parse_args(argc, argv, options)) {
        print_usage();
        return 1;
    }

    const bool json_metrics_mode = options.stage_metrics_json;
    ScopedStdoutSilencer stdout_silencer(json_metrics_mode);

    if (!aletheia::frontend_is_available(options.frontend)) {
        if (!json_metrics_mode) {
            std::cerr << "idump: --frontend=" << aletheia::frontend_kind_name(options.frontend)
                      << " is unavailable: " << aletheia::frontend_unavailable_reason(options.frontend) << "\n";
        }
        return 1;
    }

    const bool trace_pass_pseudocode = !json_metrics_mode && should_trace_pass_pseudocode(options);

    if (!json_metrics_mode && !detect_headless(options)) {
        std::cerr << "idump: headless mode not explicit; proceeding (set --headless or ALETHEIA_HEADLESS=1).\n";
    }

    ida::database::RuntimeOptions runtime_options;
    runtime_options.quiet = true;
    auto init_res = ida::database::init(runtime_options);
    if (!init_res) {
        if (!json_metrics_mode) {
            std::cerr << "idump: failed to initialize idalib runtime.\n";
        }
        return 1;
    }

    auto open_res = ida::database::open(options.input_binary, ida::database::OpenMode::Analyze);
    if (!open_res) {
        if (!json_metrics_mode) {
            std::cerr << "idump: failed to open input binary: " << options.input_binary << "\n";
        }
        return 1;
    }

    std::filesystem::path output_path = options.output_path.empty()
        ? std::filesystem::path(options.input_binary).replace_extension(".c")
        : std::filesystem::path(options.output_path);

    std::ofstream out(output_path);
    if (!out.is_open()) {
        std::cerr << "idump: failed to open output file: " << output_path << "\n";
        ida::database::close(false);
        return 1;
    }

    idiomata::IdiomMatcher matcher;

    std::size_t total_functions = 0;
    std::size_t decompiled_functions = 0;
    aletheia::FrontendSupportReport frontend_support_total;
    std::unordered_map<std::string, std::size_t> frontend_diagnostic_counts;
    std::unordered_map<std::string, PortableGlobalBinding> portable_global_bindings;
    RunScopedDebugState run_debug_state;
    std::vector<JsonMetricsPerFunction> json_metrics_functions;
    std::vector<EmittedFunction> emitted_functions;

    std::vector<ida::Address> function_addresses;
    for (const auto& fn : ida::function::all()) {
        function_addresses.push_back(fn.start());
    }
    SignatureDiscoveryResult signature_discovery;
    signature_discovery = discover_function_signatures(
        function_addresses, matcher, options.frontend);
    const FunctionSignatureRegistry* run_signatures =
        &signature_discovery.signatures;

    for (const auto& fn : ida::function::all()) {
        ++total_functions;
        const bool declaration_only = options.portable_c
            && is_external_linkage_thunk(fn);
        bool ok = false;
        std::string error_message;
        DecompileDebugOutput debug_output;
        auto lines = decompile_function(
            fn.start(),
            matcher,
            ok,
            error_message,
            trace_pass_pseudocode,
            options,
            &run_debug_state,
            &debug_output,
            run_signatures);

        frontend_support_total.implemented_ops += debug_output.frontend_support.implemented_ops;
        frontend_support_total.fallback_ops += debug_output.frontend_support.fallback_ops;
        frontend_support_total.unsupported_ops += debug_output.frontend_support.unsupported_ops;
        for (const auto& diagnostic : debug_output.frontend_diagnostics) {
            if (diagnostic.severity == aletheia::FrontendDiagnosticSeverity::Warning
                || diagnostic.severity == aletheia::FrontendDiagnosticSeverity::Error) {
                ++frontend_diagnostic_counts[
                    diagnostic.code + ": " + diagnostic.message];
            }
        }
        for (const PortableGlobalBinding& binding : debug_output.portable_globals) {
            auto [it, inserted] = portable_global_bindings.try_emplace(
                binding.name, binding);
            if (!inserted) {
                it->second.required_size = std::max(
                    it->second.required_size, binding.required_size);
                if (it->second.address == 0 && binding.address != 0) {
                    it->second = binding;
                }
            }
        }

        if (ok && options.portable_c && !declaration_only) {
            if (auto marker = portable_c_unresolved_marker(lines)) {
                ok = false;
                error_message = "portable-C output contains unresolved semantic marker '"
                    + *marker + "'";
            }
        }

        if (!ok) {
            if (!json_metrics_mode) {
                std::cerr << "idump: decompilation failed at 0x" << std::hex << fn.start()
                          << std::dec << ": " << error_message << '\n';
            }
            emitted_functions.push_back(EmittedFunction{
                fn.start(), false, std::move(error_message), {}, false});
            continue;
        }

        if (json_metrics_mode) {
            JsonMetricsPerFunction metrics_row;
            metrics_row.function_address = static_cast<std::uint64_t>(fn.start());
            metrics_row.function_name = "sub_" + std::to_string(static_cast<std::uint64_t>(fn.start()));
            metrics_row.stages = std::move(debug_output.stage_metrics);
            json_metrics_functions.push_back(std::move(metrics_row));
        }

        ++decompiled_functions;
        emitted_functions.push_back(EmittedFunction{
            fn.start(), true, {}, std::move(lines), declaration_only});
    }

    std::unordered_set<std::string> materialized_global_names;
    if (options.portable_c) {
        emit_portable_c_preamble(
            out,
            emitted_functions,
            portable_global_bindings,
            &materialized_global_names);
    }
    for (const EmittedFunction& function : emitted_functions) {
        if (!function.ok) {
            out << "/* decompilation failed at " << std::hex << function.address << std::dec
                << ": " << aletheia::frontend::sanitize_c_block_comment_text(
                    function.error_message)
                << " */\n\n";
            continue;
        }
        if (function.declaration_only) {
            continue;
        }
        for (const std::string& line : function.lines) {
            if (options.portable_c
                && is_materialized_global_declaration(
                    line, materialized_global_names)) {
                continue;
            }
            out << line << '\n';
        }
        out << '\n';
    }

    out.flush();
    ida::database::close(false);

    if (json_metrics_mode) {
        std::sort(
            json_metrics_functions.begin(),
            json_metrics_functions.end(),
            [](const JsonMetricsPerFunction& lhs, const JsonMetricsPerFunction& rhs) {
                return lhs.function_address < rhs.function_address;
            });
        std::vector<aletheia::debug::FunctionStageMetrics> functions;
        functions.reserve(json_metrics_functions.size());
        for (const auto& fn : json_metrics_functions) {
            aletheia::debug::FunctionStageMetrics row;
            row.function_name = fn.function_name;
            row.function_address = fn.function_address;
            row.stages = fn.stages;
            functions.push_back(std::move(row));
        }
        stdout_silencer.restore();
        std::cout << aletheia::debug::format_stage_metrics_report_json(
            options.input_binary,
            total_functions,
            decompiled_functions,
            functions);
        return decompiled_functions == total_functions ? 0 : 2;
    }

    if (options.frontend == aletheia::FrontendKind::Pcode) {
        std::cerr << "idump: pcode signature registry functions="
                  << signature_discovery.signatures.size()
                  << " passes=" << signature_discovery.passes
                  << " converged=" << (signature_discovery.converged ? "yes" : "no")
                  << '\n';
        std::cerr << "idump: pcode support implemented=" << frontend_support_total.implemented_ops
                  << " fallback=" << frontend_support_total.fallback_ops
                  << " unsupported=" << frontend_support_total.unsupported_ops << '\n';
        std::vector<std::pair<std::string, std::size_t>> ordered_diagnostics(
            frontend_diagnostic_counts.begin(), frontend_diagnostic_counts.end());
        std::sort(ordered_diagnostics.begin(), ordered_diagnostics.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.second != rhs.second
                    ? lhs.second > rhs.second : lhs.first < rhs.first;
            });
        for (const auto& [diagnostic, count] : ordered_diagnostics) {
            std::cerr << "idump: pcode diagnostic count=" << count
                      << " " << diagnostic << '\n';
        }
    }
    std::cerr << "idump: wrote " << decompiled_functions << "/" << total_functions
              << " functions to " << output_path << "\n";
    return decompiled_functions == total_functions ? 0 : 2;
}
