#include "lifter.hpp"
#include <ida/lines.hpp>
#include <ida/function.hpp>
#include <ida/database.hpp>
#include <ida/type.hpp>
#include <ida/segment.hpp>
#include <ida/xref.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include "pipeline/pipeline.hpp"
#include "structures/types.hpp"

namespace aletheia {

namespace {

std::string hex_address_name(ida::Address ea) {
    std::ostringstream oss;
    oss << std::hex << ea;
    return "g_" + oss.str();
}

std::string sanitize_identifier(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return c < 32 || c > 126;
    }), text.end());

    if (auto ptr_pos = text.find("ptr "); ptr_pos != std::string::npos) {
        text = text.substr(ptr_pos + 4);
    }
    if (auto colon_pos = text.rfind(':'); colon_pos != std::string::npos) {
        text = text.substr(colon_pos + 1);
    }

    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) || c == '[' || c == ']';
    }), text.end());

    for (char& c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_')) {
            c = '_';
        }
    }

    if (text.empty()) {
        return text;
    }
    const unsigned char first = static_cast<unsigned char>(text.front());
    if (std::isdigit(first)) {
        text = "g_" + text;
    }
    return text;
}

std::string global_name_from_operand(const ida::instruction::Operand& op, ida::Address insn_addr) {
    auto op_text = ida::instruction::operand_text(insn_addr, op.index());
    if (op_text) {
        std::string sanitized = sanitize_identifier(*op_text);
        if (!sanitized.empty()) {
            return sanitized;
        }
    }
    return hex_address_name(op.value());
}

std::optional<std::string> normalize_ida_stack_symbol(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    auto parse_suffix_hex = [&](std::string_view prefix, std::string_view out_prefix) -> std::optional<std::string> {
        if (!text.starts_with(prefix)) {
            return std::nullopt;
        }

        const std::string_view suffix{text.c_str() + prefix.size(), text.size() - prefix.size()};
        if (suffix.empty()) {
            return std::nullopt;
        }

        std::uint64_t value = 0;
        for (char c : suffix) {
            int digit = 0;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = 10 + (c - 'a');
            } else {
                return std::nullopt;
            }
            value = (value << 4) + static_cast<std::uint64_t>(digit);
        }

        return std::string(out_prefix) + std::to_string(value);
    };

    if (auto local = parse_suffix_hex("var_", "local_")) {
        return local;
    }
    if (auto arg = parse_suffix_hex("arg_", "arg_")) {
        return arg;
    }
    return std::nullopt;
}

std::string to_lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool is_identifier_char(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

bool contains_register_token(std::string_view text, std::string_view token) {
    if (token.empty()) {
        return false;
    }
    std::size_t pos = text.find(token);
    while (pos != std::string_view::npos) {
        const bool left_ok = (pos == 0) || !is_identifier_char(text[pos - 1]);
        const std::size_t end = pos + token.size();
        const bool right_ok = (end >= text.size()) || !is_identifier_char(text[end]);
        if (left_ok && right_ok) {
            return true;
        }
        pos = text.find(token, pos + 1);
    }
    return false;
}

std::optional<std::int64_t> parse_signed_displacement(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '+' && text[i] != '-') {
            continue;
        }

        const bool negative = text[i] == '-';
        std::size_t j = i + 1;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '#' || text[j] == ',')) {
            ++j;
        }
        if (j >= text.size()) {
            continue;
        }

        int base = 10;
        if (j + 1 < text.size() && text[j] == '0' && text[j + 1] == 'x') {
            base = 16;
            j += 2;
        }

        std::size_t start = j;
        while (j < text.size()) {
            const unsigned char ch = static_cast<unsigned char>(text[j]);
            const bool ok = (base == 16)
                ? std::isxdigit(ch)
                : std::isdigit(ch);
            if (!ok) {
                break;
            }
            ++j;
        }
        if (j == start) {
            continue;
        }

        std::int64_t value = 0;
        for (std::size_t k = start; k < j; ++k) {
            const char c = text[k];
            int digit = 0;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = 10 + (c - 'a');
            } else if (c >= 'A' && c <= 'F') {
                digit = 10 + (c - 'A');
            } else {
                break;
            }
            value = value * base + digit;
        }

        return negative ? -value : value;
    }

    // Fallback for forms like "[sp, #0x10]" where there is no explicit + sign.
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '#') {
            continue;
        }

        std::size_t j = i + 1;
        bool negative = false;
        if (j < text.size() && (text[j] == '+' || text[j] == '-')) {
            negative = text[j] == '-';
            ++j;
        }

        int base = 10;
        if (j + 1 < text.size() && text[j] == '0' && text[j + 1] == 'x') {
            base = 16;
            j += 2;
        }

        std::size_t start = j;
        while (j < text.size()) {
            const unsigned char ch = static_cast<unsigned char>(text[j]);
            const bool ok = (base == 16)
                ? std::isxdigit(ch)
                : std::isdigit(ch);
            if (!ok) {
                break;
            }
            ++j;
        }
        if (j == start) {
            continue;
        }

        std::int64_t value = 0;
        for (std::size_t k = start; k < j; ++k) {
            const char c = text[k];
            int digit = 0;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = 10 + (c - 'a');
            } else if (c >= 'A' && c <= 'F') {
                digit = 10 + (c - 'A');
            }
            value = value * base + digit;
        }

        return negative ? -value : value;
    }

    return std::nullopt;
}

std::optional<std::string> stack_slot_name_from_operand_text(std::string operand_text) {
    operand_text.erase(std::remove_if(operand_text.begin(), operand_text.end(), [](unsigned char c) {
        return c < 32 || c > 126;
    }), operand_text.end());

    std::string t = to_lower_ascii(std::move(operand_text));
    if (t.find('[') == std::string::npos && t.find(']') == std::string::npos) {
        return normalize_ida_stack_symbol(t);
    }

    const bool frame_based =
        contains_register_token(t, "rbp") ||
        contains_register_token(t, "ebp") ||
        contains_register_token(t, "bp") ||
        contains_register_token(t, "x29") ||
        contains_register_token(t, "fp");

    const bool sp_based =
        contains_register_token(t, "rsp") ||
        contains_register_token(t, "esp") ||
        contains_register_token(t, "sp");

    if (!frame_based && !sp_based) {
        return std::nullopt;
    }

    const std::int64_t disp = parse_signed_displacement(t).value_or(0);
    const auto abs_disp = static_cast<std::uint64_t>(disp < 0 ? -disp : disp);

    if (frame_based) {
        if (disp < 0) {
            return "local_" + std::to_string(abs_disp);
        }
        if (disp > 0) {
            return "arg_" + std::to_string(abs_disp);
        }
        return "local_0";
    }

    if (disp < 0) {
        return "local_m" + std::to_string(abs_disp);
    }
    return "local_" + std::to_string(abs_disp);
}

bool is_conditional_branch_mnemonic(std::string_view mnemonic) {
    static constexpr std::string_view kX86Jcc[] = {
        "je", "jz", "jne", "jnz", "jg", "jnle", "jge", "jnl", "jl", "jnge", "jle", "jng",
        "ja", "jnbe", "jae", "jnb", "jb", "jnae", "jbe", "jna", "js", "jns", "jo", "jno",
        "jp", "jpe", "jnp", "jpo"
    };
    for (auto m : kX86Jcc) {
        if (mnemonic == m) {
            return true;
        }
    }

    if (mnemonic == "cbz" || mnemonic == "cbnz" || mnemonic == "tbz" || mnemonic == "tbnz") {
        return true;
    }
    if (mnemonic.size() > 2 && mnemonic[0] == 'b' && mnemonic[1] == '.') {
        return true;
    }
    return false;
}

std::optional<ida::Address> branch_target_from_instruction(const ida::instruction::Instruction& insn) {
    const std::string mnemonic = to_lower_ascii(insn.mnemonic());
    if (!is_conditional_branch_mnemonic(mnemonic)) {
        return std::nullopt;
    }

    std::size_t target_operand_index = 0;
    if (mnemonic == "cbz" || mnemonic == "cbnz") {
        target_operand_index = 1;
    } else if (mnemonic == "tbz" || mnemonic == "tbnz") {
        target_operand_index = 2;
    }

    const auto& operands = insn.operands();
    if (target_operand_index >= operands.size()) {
        return std::nullopt;
    }

    const auto& op = operands[target_operand_index];
    const bool has_target_value =
        op.is_immediate() ||
        op.is_memory() ||
        op.type() == ida::instruction::OperandType::NearAddress ||
        op.type() == ida::instruction::OperandType::FarAddress;
    if (!has_target_value) {
        return std::nullopt;
    }

    return static_cast<ida::Address>(op.value());
}

std::optional<ida::Address> infer_taken_target_from_xrefs(
    ida::Address branch_address,
    ida::Address succ0_start,
    ida::Address succ1_start) {
    auto refs_res = ida::xref::code_refs_from(branch_address);
    if (!refs_res) {
        return std::nullopt;
    }

    std::optional<ida::Address> jump_target;
    std::optional<ida::Address> flow_target;

    for (const auto& ref : *refs_res) {
        if (ref.to != succ0_start && ref.to != succ1_start) {
            continue;
        }

        if (ref.type == ida::xref::ReferenceType::JumpNear ||
            ref.type == ida::xref::ReferenceType::JumpFar) {
            jump_target = ref.to;
        } else if (ref.type == ida::xref::ReferenceType::Flow) {
            flow_target = ref.to;
        }
    }

    if (jump_target.has_value()) {
        return jump_target;
    }
    if (flow_target.has_value()) {
        if (*flow_target == succ0_start) {
            return succ1_start;
        }
        if (*flow_target == succ1_start) {
            return succ0_start;
        }
    }
    return std::nullopt;
}

} // namespace

namespace {

/// Calling convention parameter register tables.
/// x86-64 System V ABI: rdi, rsi, rdx, rcx, r8, r9
constexpr std::string_view kX86_64_SysV_IntRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
/// x86-64 Windows (fastcall): rcx, rdx, r8, r9
constexpr std::string_view kX86_64_Win_IntRegs[] = {"rcx", "rdx", "r8", "r9"};
/// ARM64 (AAPCS): x0-x7
constexpr std::string_view kARM64_IntRegs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};

/// Detect the architecture from the processor name. Returns "x86_64", "arm64", or "".
std::string detect_arch() {
    auto proc = ida::database::processor_name();
    if (!proc) return "";
    std::string p = to_lower_ascii(*proc);
    if (p.find("arm") != std::string::npos || p.find("aarch64") != std::string::npos) {
        return "arm64";
    }
    if (p.find("metapc") != std::string::npos || p.find("x86") != std::string::npos ||
        p.find("pc") != std::string::npos) {
        return "x86_64";
    }
    return "";
}

/// Get parameter registers for the detected architecture.
/// Returns a span-like pair of (pointer, count).
std::pair<const std::string_view*, std::size_t> param_register_table() {
    static std::string cached_arch;
    if (cached_arch.empty()) {
        cached_arch = detect_arch();
    }
    if (cached_arch == "arm64") {
        return {kARM64_IntRegs, std::size(kARM64_IntRegs)};
    }
    if (cached_arch == "x86_64") {
        // Default to System V ABI (Linux/macOS). Windows detection could be
        // added by checking the input file's format, but System V is more common.
        return {kX86_64_SysV_IntRegs, std::size(kX86_64_SysV_IntRegs)};
    }
    return {nullptr, 0};
}

} // namespace (parameter tables)

void Lifter::populate_task_signature(DecompilerTask& task) {
    auto ea = task.function_address();
    
    // Default name
    std::string name = "sub_" + std::to_string(ea);
    
    auto name_res = ida::function::name_at(ea);
    if (name_res) {
        name = *name_res;
    }
    task.set_function_name(name);

    auto type_res = ida::type::retrieve(ea);
    std::size_t param_count = 0;

    if (type_res) {
        auto& type_info = *type_res;
        TypeParser parser;
        
        if (type_info.is_function()) {
            auto ret_res = type_info.function_return_type();
            auto args_res = type_info.function_argument_types();
            
            TypePtr ret_type = CustomType::void_type();
            if (ret_res) {
                auto ret_str = ret_res->to_string();
                if (ret_str) ret_type = parser.parse(*ret_str);
            }

            std::vector<TypePtr> params;
            if (args_res) {
                for (const auto& arg_type : *args_res) {
                    auto arg_str = arg_type.to_string();
                    if (arg_str) params.push_back(parser.parse(*arg_str));
                }
            }
            
            task.set_function_type(std::make_shared<const FunctionTypeDef>(ret_type, params));
            param_count = params.size();

            // Build parameter register -> parameter info mapping.
            auto [reg_table, reg_count] = param_register_table();
            param_register_map_.clear();
            if (reg_table) {
                for (std::size_t i = 0; i < param_count && i < reg_count; ++i) {
                    std::string reg_name(reg_table[i]);
                    param_register_map_[reg_name] = static_cast<int>(i);
                    
                    DecompilerTask::ParameterInfo info;
                    info.name = "a" + std::to_string(i + 1);
                    info.index = static_cast<int>(i);
                    info.type = (i < params.size()) ? params[i] : nullptr;
                    task.set_parameter_register(reg_name, std::move(info));
                }
            }
        } else {
            auto type_str = type_info.to_string();
            if (type_str) {
                task.set_function_type(parser.parse(*type_str));
            }
        }
    }

    // Also check for register variables defined by the user in IDA.
    auto regvars = ida::function::register_variables(ea);
    if (regvars) {
        for (const auto& rv : *regvars) {
            if (!rv.user_name.empty()) {
                std::string canonical = to_lower_ascii(rv.canonical_name);
                // If this register is a parameter, update the display name.
                auto it = param_register_map_.find(canonical);
                if (it != param_register_map_.end()) {
                    DecompilerTask::ParameterInfo info;
                    info.name = rv.user_name;
                    info.index = it->second;
                    info.type = nullptr; // Already set from function type.
                    task.set_parameter_register(canonical, std::move(info));
                }
            }
        }
    }
}

void Lifter::populate_frame_layout(ida::Address function_address) {
    frame_layout_ = FrameLayout{}; // Reset.
    
    auto frame_res = ida::function::frame(function_address);
    if (!frame_res) {
        return; // No frame info available; fall back to text heuristics.
    }

    const auto& sf = *frame_res;
    frame_layout_.local_size = sf.local_variables_size();
    frame_layout_.regs_size = sf.saved_registers_size();
    frame_layout_.args_size = sf.arguments_size();
    frame_layout_.total_size = sf.total_size();
    frame_layout_.valid = true;

    // Build offset-to-variable map.
    // IDA frame offsets are relative to the frame structure base.
    // The frame structure is laid out as:
    //   [0 .. local_size)         -> local variables
    //   [local_size .. local_size + regs_size) -> saved registers
    //   [local_size + regs_size .. total_size) -> arguments
    // To convert to frame-pointer-relative (FP = base + local_size + regs_size):
    //   fp_relative = byte_offset - (local_size + regs_size)
    // So locals have negative FP offsets, arguments have non-negative FP offsets.
    const auto fp_base = static_cast<std::int64_t>(frame_layout_.local_size + frame_layout_.regs_size);

    for (const auto& fv : sf.variables()) {
        if (fv.is_special) continue; // Skip __return_address, __saved_registers.
        
        const auto fp_offset = static_cast<std::int64_t>(fv.byte_offset) - fp_base;
        frame_layout_.offset_to_var[fp_offset] = fv;
    }
}

std::optional<std::string> Lifter::resolve_frame_variable(std::int64_t frame_offset,
                                                           std::size_t access_size) const {
    if (!frame_layout_.valid) {
        return std::nullopt;
    }

    // Exact match first.
    auto it = frame_layout_.offset_to_var.find(frame_offset);
    if (it != frame_layout_.offset_to_var.end() && !it->second.name.empty()) {
        return it->second.name;
    }

    // Try to find a variable that contains this offset (subfield access).
    for (const auto& [off, fv] : frame_layout_.offset_to_var) {
        if (fv.name.empty()) continue;
        if (frame_offset >= off &&
            static_cast<std::size_t>(frame_offset - off) < fv.byte_size) {
            // Inside this variable's range.
            if (frame_offset == off) {
                return fv.name; // Exact start.
            }
            // Subfield: append offset suffix.
            return fv.name + "_" + std::to_string(frame_offset - off);
        }
    }

    return std::nullopt;
}

void Lifter::tag_variable(Variable* var, ida::Address insn_addr) const {
    if (!var) return;

    const std::string& vname = var->name();

    // Check if this is a parameter register.
    std::string lower_name = to_lower_ascii(vname);
    auto pit = param_register_map_.find(lower_name);
    if (pit != param_register_map_.end()) {
        var->set_kind(VariableKind::Parameter);
        var->set_parameter_index(pit->second);
        return;
    }

    // Check if this is a recognized stack variable name.
    if (vname.starts_with("local_")) {
        var->set_kind(VariableKind::StackLocal);
        // Try to extract offset from name (e.g., "local_16" -> -16).
        auto suffix = vname.substr(6);
        if (!suffix.empty() && suffix[0] != 'm') {
            try {
                auto offset = std::stoll(suffix);
                var->set_stack_offset(-offset); // Locals are at negative FP offsets.
            } catch (...) {}
        }
    } else if (vname.starts_with("arg_")) {
        var->set_kind(VariableKind::StackArgument);
        auto suffix = vname.substr(4);
        try {
            auto offset = std::stoll(suffix);
            var->set_stack_offset(offset);
        } catch (...) {}
    }
}

Variable* Lifter::resolve_sp_relative_slot(ida::Address insn_addr,
                                           std::int64_t sp_adjust_bytes,
                                           std::size_t access_size) {
    const std::size_t width = access_size > 0 ? access_size : static_cast<std::size_t>(8);

    std::int64_t fp_offset = 0;
    bool have_fp_offset = false;

    if (frame_layout_.valid) {
        auto sp_delta = ida::function::sp_delta_at(insn_addr);
        if (sp_delta.has_value()) {
            const auto fp_base = static_cast<std::int64_t>(
                frame_layout_.local_size + frame_layout_.regs_size);
            fp_offset = *sp_delta + sp_adjust_bytes + fp_base;
            have_fp_offset = true;
        }
    }

    std::string slot_name;
    if (have_fp_offset) {
        if (auto resolved = resolve_frame_variable(fp_offset, width)) {
            slot_name = *resolved;
        }
    }

    auto abs_i64 = [](std::int64_t value) -> std::uint64_t {
        return value < 0
            ? static_cast<std::uint64_t>(-(value + 1)) + 1ULL
            : static_cast<std::uint64_t>(value);
    };

    if (slot_name.empty()) {
        if (have_fp_offset) {
            const std::uint64_t abs_off = abs_i64(fp_offset);
            if (fp_offset < 0) {
                slot_name = "local_" + std::to_string(abs_off);
            } else if (fp_offset > 0) {
                slot_name = "arg_" + std::to_string(abs_off);
            } else {
                slot_name = "local_0";
            }
        } else {
            std::int64_t sp_key = sp_adjust_bytes;
            if (auto sp_delta = ida::function::sp_delta_at(insn_addr); sp_delta.has_value()) {
                sp_key += *sp_delta;
            }
            const std::uint64_t abs_off = abs_i64(sp_key);
            slot_name = sp_key < 0
                ? "sp_local_" + std::to_string(abs_off)
                : "sp_arg_" + std::to_string(abs_off);
        }
    }

    auto* slot = arena_.create<Variable>(slot_name, width);
    slot->set_aliased(true);

    if (have_fp_offset) {
        slot->set_stack_offset(fp_offset);
        slot->set_kind(fp_offset >= 0 ? VariableKind::StackArgument : VariableKind::StackLocal);

        auto it = frame_layout_.offset_to_var.find(fp_offset);
        if (it != frame_layout_.offset_to_var.end() && it->second.byte_size > 0) {
            slot->set_ir_type(std::make_shared<Integer>(it->second.byte_size * 8, false));
        }
    } else {
        slot->set_kind(VariableKind::StackLocal);
    }

    if (!slot->ir_type()) {
        slot->set_ir_type(std::make_shared<Integer>(width * 8, false));
    }

    return slot;
}

ida::Result<std::unique_ptr<ControlFlowGraph>> Lifter::lift_function(
    ida::Address function_address,
    std::vector<idiomata::IdiomTag>* idiom_tags_out) {

    // Cache function address and populate frame layout for stack recovery.
    current_function_ea_ = function_address;
    populate_frame_layout(function_address);

    auto flowchart_res = ida::graph::flowchart(function_address);
    if (!flowchart_res) {
        return std::unexpected(flowchart_res.error());
    }

    auto cfg = std::make_unique<ControlFlowGraph>();
    std::unordered_map<int, BasicBlock*> block_map;

    int id = 0;
    for (const auto& ida_block : *flowchart_res) {
        BasicBlock* block = arena_.create<BasicBlock>(id);
        block_map[id] = block;
        cfg->add_block(block);
        
        if (id == 0) {
            cfg->set_entry_block(block);
        }
        id++;
    }

    id = 0;
    for (const auto& ida_block : *flowchart_res) {
        BasicBlock* block = block_map[id];

        if (idiom_tags_out != nullptr) {
            auto block_tags = idiom_matcher_.match_block(ida_block);
            idiom_tags_out->insert(
                idiom_tags_out->end(),
                std::make_move_iterator(block_tags.begin()),
                std::make_move_iterator(block_tags.end()));
        }

        ida::Address last_decoded_addr = ida::BadAddress;

        // Lift instructions
        for (ida::Address addr = ida_block.start; addr < ida_block.end; ) {
            auto insn_res = ida::instruction::decode(addr);
            if (!insn_res) {
                addr += 1;
                continue;
            }

            last_decoded_addr = addr;
            
            Instruction* lifted_insn = lift_instruction(*insn_res);
            if (lifted_insn) {
                block->add_instruction(lifted_insn);
            }
            
            addr += insn_res->size();
        }

        // Lift edges properly
        const bool likely_switch_dispatch =
            ida_block.successors.size() > 2 &&
            !block->instructions().empty() &&
            isa<IndirectBranch>(block->instructions().back());

        if (likely_switch_dispatch) {
            std::vector<int> ordered_targets;
            std::unordered_map<int, std::vector<std::int64_t>> target_case_values;

            for (std::size_t case_index = 0; case_index < ida_block.successors.size(); ++case_index) {
                const int succ_id = ida_block.successors[case_index];
                if (!target_case_values.contains(succ_id)) {
                    ordered_targets.push_back(succ_id);
                }
                target_case_values[succ_id].push_back(static_cast<std::int64_t>(case_index));
            }

            for (int succ_id : ordered_targets) {
                if (!block_map.contains(succ_id)) {
                    continue;
                }
                BasicBlock* target = block_map[succ_id];
                auto case_values_it = target_case_values.find(succ_id);
                std::vector<std::int64_t> case_values;
                if (case_values_it != target_case_values.end()) {
                    case_values = case_values_it->second;
                }

                Edge* edge = arena_.create<SwitchEdge>(block, target, std::move(case_values));
                block->add_successor(edge);
                target->add_predecessor(edge);
            }
        } else if (ida_block.successors.size() == 2) {
            BasicBlock* target0 = block_map.contains(ida_block.successors[0]) ? block_map[ida_block.successors[0]] : nullptr;
            BasicBlock* target1 = block_map.contains(ida_block.successors[1]) ? block_map[ida_block.successors[1]] : nullptr;

            BasicBlock* true_target = target0;
            BasicBlock* false_target = target1;

            if (target0 && target1) {
                const auto succ0_id = ida_block.successors[0];
                const auto succ1_id = ida_block.successors[1];

                std::optional<ida::Address> succ0_start;
                std::optional<ida::Address> succ1_start;
                if (succ0_id >= 0 && static_cast<std::size_t>(succ0_id) < flowchart_res->size()) {
                    succ0_start = (*flowchart_res)[succ0_id].start;
                }
                if (succ1_id >= 0 && static_cast<std::size_t>(succ1_id) < flowchart_res->size()) {
                    succ1_start = (*flowchart_res)[succ1_id].start;
                }

                std::optional<ida::Address> taken_target;
                if (last_decoded_addr != ida::BadAddress && succ0_start.has_value() && succ1_start.has_value()) {
                    taken_target = infer_taken_target_from_xrefs(last_decoded_addr, *succ0_start, *succ1_start);
                    if (!taken_target.has_value()) {
                        auto last_insn = ida::instruction::decode(last_decoded_addr);
                        if (last_insn) {
                            taken_target = branch_target_from_instruction(*last_insn);
                        }
                    }

                    if (taken_target.has_value()) {
                        if (*taken_target == *succ1_start) {
                            true_target = target1;
                            false_target = target0;
                        } else if (*taken_target == *succ0_start) {
                            true_target = target0;
                            false_target = target1;
                        }
                    }
                }
            }

            if (true_target) {
                Edge* e_true = arena_.create<Edge>(block, true_target, EdgeType::True);
                block->add_successor(e_true);
                true_target->add_predecessor(e_true);
            }
            if (false_target && false_target != true_target) {
                Edge* e_false = arena_.create<Edge>(block, false_target, EdgeType::False);
                block->add_successor(e_false);
                false_target->add_predecessor(e_false);
            }
        } else {
            for (int succ_id : ida_block.successors) {
                if (block_map.contains(succ_id)) {
                    BasicBlock* target = block_map[succ_id];
                    Edge* edge = arena_.create<Edge>(block, target, EdgeType::Unconditional);
                    block->add_successor(edge);
                    target->add_predecessor(edge);
                }
            }
        }
        
        id++;
    }

    return cfg;
}

std::vector<Expression*> Lifter::lift_operands(const ida::instruction::Instruction& insn) {
    std::vector<Expression*> operands;
    for (const auto& op : insn.operands()) {
        if (op.type() == ida::instruction::OperandType::None) continue;
        operands.push_back(lift_operand(op, insn.address()));
    }
    return operands;
}

Expression* Lifter::lift_operand(const ida::instruction::Operand& op, ida::Address insn_addr) {
    Expression* expr = nullptr;
    if (op.is_register()) {
        std::string reg_name = op.register_name();
        std::string lower_reg = to_lower_ascii(reg_name);
        
        // Check if this register is a parameter register.
        auto pit = param_register_map_.find(lower_reg);
        if (pit != param_register_map_.end()) {
            auto* var = arena_.create<Variable>(reg_name, op.byte_width());
            var->set_kind(VariableKind::Parameter);
            var->set_parameter_index(pit->second);
            expr = var;
        } else {
            expr = arena_.create<Variable>(reg_name, op.byte_width());
        }
    } else if (op.is_immediate()) {
        expr = arena_.create<Constant>(op.value(), op.byte_width());
    } else if (op.type() == ida::instruction::OperandType::NearAddress ||
               op.type() == ida::instruction::OperandType::FarAddress) {
        const std::uint64_t target = op.target_address() != ida::BadAddress
            ? static_cast<std::uint64_t>(op.target_address())
            : op.value();
        const std::size_t width = op.byte_width() > 0 ? static_cast<std::size_t>(op.byte_width()) : 8U;
        expr = arena_.create<Constant>(target, width);
    } else if (op.is_memory()) {
        const std::size_t width = op.byte_width() > 0 ? static_cast<std::size_t>(op.byte_width()) : 8U;
        if (op.type() == ida::instruction::OperandType::MemoryDirect) {
            const std::string global_name = global_name_from_operand(op, insn_addr);
            bool is_const = false;
            if (auto seg = ida::segment::at(op.value())) {
                is_const = !seg->permissions().write;
            }

            auto* initial_value = arena_.create<Constant>(0, width);
            auto* global = arena_.create<GlobalVariable>(global_name, width, initial_value, is_const);
            global->set_ir_type(std::make_shared<Integer>(width * 8, false));

            expr = arena_.create<Operation>(
                OperationType::deref,
                std::vector<Expression*>{global},
                width);
        } else {
            // Memory phrase/displacement: try IDA frame resolution first,
            // then fall back to text heuristic.
            auto op_text = ida::instruction::operand_text(insn_addr, op.index());
            if (op_text) {
                std::string text = *op_text;
                std::string lower_text = to_lower_ascii(text);

                // Strategy 1: Use IDA frame API if available.
                // Detect frame-pointer or stack-pointer reference and resolve
                // the offset through the frame variable map.
                bool resolved = false;
                if (frame_layout_.valid) {
                    const bool is_fp_based =
                        contains_register_token(lower_text, "rbp") ||
                        contains_register_token(lower_text, "ebp") ||
                        contains_register_token(lower_text, "bp") ||
                        contains_register_token(lower_text, "x29") ||
                        contains_register_token(lower_text, "fp");
                    const bool is_sp_based =
                        contains_register_token(lower_text, "rsp") ||
                        contains_register_token(lower_text, "esp") ||
                        contains_register_token(lower_text, "sp");

                    if (is_fp_based || is_sp_based) {
                        auto disp = parse_signed_displacement(lower_text);
                        std::int64_t fp_offset = disp.value_or(0);

                        // For SP-based references, adjust using SP delta to get
                        // frame-pointer-relative offset.
                        if (is_sp_based && !is_fp_based) {
                            auto sp_delta = ida::function::sp_delta_at(insn_addr);
                            if (sp_delta) {
                                // sp_delta_at returns the SP change from function entry.
                                // FP = initial_SP - (local_size + regs_size)
                                // SP at insn = initial_SP + sp_delta (sp_delta is negative)
                                // actual_addr = SP_at_insn + disp = initial_SP + sp_delta + disp
                                // FP_relative = actual_addr - FP
                                //             = (initial_SP + sp_delta + disp) - (initial_SP - fp_base)
                                //             = sp_delta + disp + fp_base
                                const auto fp_base = static_cast<std::int64_t>(
                                    frame_layout_.local_size + frame_layout_.regs_size);
                                fp_offset = *sp_delta + fp_offset + fp_base;
                            }
                        }

                        // Try to resolve via IDA frame variable map.
                        auto frame_name = resolve_frame_variable(fp_offset, width);
                        if (frame_name) {
                            auto* slot = arena_.create<Variable>(*frame_name, width);
                            slot->set_aliased(true);
                            slot->set_stack_offset(fp_offset);
                            if (fp_offset >= 0) {
                                slot->set_kind(VariableKind::StackArgument);
                            } else {
                                slot->set_kind(VariableKind::StackLocal);
                            }
                            
                            // Try to get type from frame variable.
                            auto it = frame_layout_.offset_to_var.find(fp_offset);
                            if (it != frame_layout_.offset_to_var.end() && it->second.byte_size > 0) {
                                slot->set_ir_type(std::make_shared<Integer>(
                                    it->second.byte_size * 8, false));
                            }
                            
                            expr = slot;
                            resolved = true;
                        }
                    }
                }

                // Strategy 2: Fall back to text-heuristic stack name parsing.
                if (!resolved) {
                    if (auto stack_name = stack_slot_name_from_operand_text(text)) {
                        auto* slot = arena_.create<Variable>(*stack_name, width);
                        slot->set_aliased(true);
                        tag_variable(slot, insn_addr);
                        expr = slot;
                    }
                }
            }

            if (!expr) {
                auto base = arena_.create<Variable>("mem_" + std::to_string(op.value()), width);
                expr = arena_.create<Operation>(OperationType::deref, std::vector<Expression*>{base}, width);
            }
        }
    } else {
        auto txt_res = ida::instruction::operand_text(insn_addr, op.index());
        if (txt_res) {
            if (auto stack_name = stack_slot_name_from_operand_text(*txt_res)) {
                auto* slot = arena_.create<Variable>(*stack_name, op.byte_width());
                slot->set_aliased(true);
                tag_variable(slot, insn_addr);
                expr = slot;
            }

            std::string clean = sanitize_identifier(*txt_res);
            if (!expr) {
                if (clean.empty()) {
                    clean = "op_" + std::to_string(op.index());
                }
                expr = arena_.create<Variable>(clean, op.byte_width());
            }
        } else {
            expr = arena_.create<Variable>("op_" + std::to_string(op.index()), op.byte_width());
        }
    }
    
    // Attach types to Variable and Constant nodes during lifting.
    if (expr && op.byte_width() > 0) {
        // Don't overwrite type if already set (e.g., from frame variable resolution).
        if (!expr->ir_type()) {
            expr->set_ir_type(std::make_shared<Integer>(op.byte_width() * 8, false));
        }
    }

    // Tag any remaining untagged variables.
    if (auto* var = dyn_cast<Variable>(expr)) {
        if (var->kind() == VariableKind::Register) {
            tag_variable(var, insn_addr);
        }
    }

    return expr;
}

// Helper: create a binary operation assignment.
// 3-operand form: dest = src1 OP src2
// 2-operand form: dest = dest OP src
Assignment* Lifter::make_binary_assign(OperationType op_type,
                                       std::vector<Expression*>& operands,
                                       ida::Address addr) {
    Assignment* assign = nullptr;
    if (operands.size() >= 3) {
        auto* rhs = arena_.create<Operation>(op_type,
            std::vector<Expression*>{operands[1], operands[2]}, operands[0]->size_bytes);
        assign = arena_.create<Assignment>(operands[0], rhs);
    } else if (operands.size() == 2) {
        auto* rhs = arena_.create<Operation>(op_type,
            std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
        assign = arena_.create<Assignment>(operands[0], rhs);
    } else if (operands.size() == 1) {
        // Unary form (e.g., NEG, NOT, INC, DEC handled separately)
        assign = arena_.create<Assignment>(operands[0], operands[0]);
    }
    if (assign) assign->set_address(addr);
    return assign;
}

// Helper: create a unary operation assignment.
// dest = OP(src)  (1 operand: dest = OP(dest))
Assignment* Lifter::make_unary_assign(OperationType op_type,
                                      std::vector<Expression*>& operands,
                                      ida::Address addr) {
    if (operands.empty()) return nullptr;
    auto* rhs = arena_.create<Operation>(op_type,
        std::vector<Expression*>{operands[0]}, operands[0]->size_bytes);
    auto* assign = arena_.create<Assignment>(operands[0], rhs);
    assign->set_address(addr);
    return assign;
}

Instruction* Lifter::lift_instruction(const ida::instruction::Instruction& insn) {
    std::string mnem = insn.mnemonic();
    
    // Normalize to lowercase for mapping
    std::string lmnem = mnem;
    for (char& c : lmnem) c = static_cast<char>(std::tolower(c));

    auto operands = lift_operands(insn);
    ida::Address addr = insn.address();

    // =====================================================================
    // NOP -- skip entirely
    // =====================================================================
    if (lmnem == "nop" || lmnem == "fnop" || lmnem == "endbr64" || lmnem == "endbr32") {
        return nullptr;
    }

    // =====================================================================
    // Return instructions
    // =====================================================================
    if (lmnem == "ret" || lmnem == "retn" || lmnem == "retf") {
        auto* ret = arena_.create<Return>(std::move(operands));
        ret->set_address(addr);
        return ret;
    }

    // =====================================================================
    // x86 conditional branch (Jcc)
    // =====================================================================
    {
        OperationType cmp = OperationType::unknown;
        // Signed comparisons
        if (lmnem == "je" || lmnem == "jz")       cmp = OperationType::eq;
        else if (lmnem == "jne" || lmnem == "jnz") cmp = OperationType::neq;
        else if (lmnem == "jl" || lmnem == "jnge") cmp = OperationType::lt;
        else if (lmnem == "jle" || lmnem == "jng") cmp = OperationType::le;
        else if (lmnem == "jg" || lmnem == "jnle") cmp = OperationType::gt;
        else if (lmnem == "jge" || lmnem == "jnl") cmp = OperationType::ge;
        // Unsigned comparisons
        else if (lmnem == "jb" || lmnem == "jnae" || lmnem == "jc")  cmp = OperationType::lt_us;
        else if (lmnem == "jbe" || lmnem == "jna")                    cmp = OperationType::le_us;
        else if (lmnem == "ja" || lmnem == "jnbe")                    cmp = OperationType::gt_us;
        else if (lmnem == "jae" || lmnem == "jnb" || lmnem == "jnc") cmp = OperationType::ge_us;
        // Sign/overflow/parity (approximate as comparisons with flags)
        else if (lmnem == "js")  cmp = OperationType::lt;   // SF=1 ~ negative
        else if (lmnem == "jns") cmp = OperationType::ge;   // SF=0 ~ non-negative
        else if (lmnem == "jo")  cmp = OperationType::neq;  // OF=1 (approximate)
        else if (lmnem == "jno") cmp = OperationType::eq;   // OF=0 (approximate)
        else if (lmnem == "jp" || lmnem == "jpe")  cmp = OperationType::eq;   // PF=1 (approximate)
        else if (lmnem == "jnp" || lmnem == "jpo") cmp = OperationType::neq;  // PF=0 (approximate)

        if (cmp != OperationType::unknown) {
            // x86 Jcc operands are branch targets, not compared values.
            // Model as condition over synthetic flags.
            Expression* lhs = arena_.create<Variable>("flags", 1);
            Expression* rhs = arena_.create<Constant>(0, 1);
            auto* cond = arena_.create<Condition>(cmp, lhs, rhs);
            auto* branch = arena_.create<Branch>(cond);
            branch->set_address(addr);
            return branch;
        }
    }

    // =====================================================================
    // ARM conditional branch instructions
    // =====================================================================
    {
        OperationType cmp = OperationType::unknown;
        if (lmnem == "b.le" || lmnem == "ble") cmp = OperationType::le;
        else if (lmnem == "b.lt" || lmnem == "blt") cmp = OperationType::lt;
        else if (lmnem == "b.ge" || lmnem == "bge") cmp = OperationType::ge;
        else if (lmnem == "b.gt" || lmnem == "bgt") cmp = OperationType::gt;
        else if (lmnem == "b.eq" || lmnem == "beq") cmp = OperationType::eq;
        else if (lmnem == "b.ne" || lmnem == "bne") cmp = OperationType::neq;
        // ARM unsigned comparisons
        else if (lmnem == "b.lo" || lmnem == "b.cc") cmp = OperationType::lt_us;
        else if (lmnem == "b.ls") cmp = OperationType::le_us;
        else if (lmnem == "b.hi") cmp = OperationType::gt_us;
        else if (lmnem == "b.hs" || lmnem == "b.cs") cmp = OperationType::ge_us;
        // ARM CBZ/CBNZ (compare and branch)
        else if (lmnem == "cbz")  cmp = OperationType::eq;
        else if (lmnem == "cbnz") cmp = OperationType::neq;
        // ARM TBZ/TBNZ (test bit and branch) -- approximate
        else if (lmnem == "tbz")  cmp = OperationType::eq;
        else if (lmnem == "tbnz") cmp = OperationType::neq;

        if (cmp != OperationType::unknown) {
            Expression* lhs = nullptr;
            Expression* rhs = nullptr;

            if (lmnem == "cbz" || lmnem == "cbnz") {
                lhs = !operands.empty() ? operands[0] : arena_.create<Variable>("flags", 1);
                rhs = arena_.create<Constant>(0, lhs->size_bytes > 0 ? lhs->size_bytes : 1);
            } else if (lmnem == "tbz" || lmnem == "tbnz") {
                lhs = !operands.empty() ? operands[0] : arena_.create<Variable>("flags", 1);
                rhs = arena_.create<Constant>(0, lhs->size_bytes > 0 ? lhs->size_bytes : 1);
            } else {
                // ARM B.cond takes target operand only; compare flags register.
                lhs = arena_.create<Variable>("flags", 1);
                rhs = arena_.create<Constant>(0, 1);
            }

            auto* cond = arena_.create<Condition>(cmp, lhs, rhs);
            auto* branch = arena_.create<Branch>(cond);
            branch->set_address(addr);
            return branch;
        }
    }

    // =====================================================================
    // Unconditional jump
    // =====================================================================
    if (lmnem == "b") {
        // Direct ARM branch target is represented by CFG edges.
        return nullptr;
    }

    if (lmnem == "jmp" || lmnem == "br") {
        // Direct jumps are represented by CFG edges; keep only computed/indirect
        // jumps as explicit IR.
        if (operands.empty()) {
            return nullptr;
        }
        if (isa<Constant>(operands[0])) {
            return nullptr;
        }

        auto* ib = arena_.create<IndirectBranch>(operands[0]);
        ib->set_address(addr);
        return ib;
    }

    // =====================================================================
    // CMP / TEST -- flag-setting without storing result
    // =====================================================================
    if (lmnem == "cmp" && operands.size() >= 2) {
        auto* op = arena_.create<Operation>(OperationType::sub,
            std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
        auto* flags = arena_.create<Variable>("flags", operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(flags, op);
        assign->set_address(addr);
        return assign;
    }
    if (lmnem == "test" && operands.size() >= 2) {
        auto* op = arena_.create<Operation>(OperationType::bit_and,
            std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
        auto* flags = arena_.create<Variable>("flags", operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(flags, op);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 CALL instruction
    // =====================================================================
    if (lmnem == "call") {
        Expression* target = !operands.empty() ? operands[0] : arena_.create<Variable>("unknown_func", 8);
        std::vector<Expression*> args(operands.begin() + (operands.empty() ? 0 : 1), operands.end());
        auto* call = arena_.create<Call>(target, std::move(args), 8);
        // Call result assigned to a synthetic return-value variable
        auto* ret_var = arena_.create<Variable>("ret", 8);
        auto* assign = arena_.create<Assignment>(ret_var, call);
        assign->set_address(addr);
        return assign;
    }
    // ARM BL (branch-and-link = call)
    if (lmnem == "bl" || lmnem == "blr") {
        Expression* target = !operands.empty() ? operands[0] : arena_.create<Variable>("unknown_func", 8);
        std::vector<Expression*> args(operands.begin() + (operands.empty() ? 0 : 1), operands.end());
        auto* call = arena_.create<Call>(target, std::move(args), 8);
        auto* ret_var = arena_.create<Variable>("x0", 8); // ARM return in x0
        auto* assign = arena_.create<Assignment>(ret_var, call);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // Arithmetic / Logic (binary operations)
    // =====================================================================
    {
        OperationType arith = OperationType::unknown;
        // x86 arithmetic
        if      (lmnem == "add")  arith = OperationType::add;
        else if (lmnem == "adc")  arith = OperationType::add_with_carry;
        else if (lmnem == "sub")  arith = OperationType::sub;
        else if (lmnem == "sbb")  arith = OperationType::sub_with_carry;
        else if (lmnem == "imul") arith = OperationType::mul;
        else if (lmnem == "and")  arith = OperationType::bit_and;
        else if (lmnem == "or")   arith = OperationType::bit_or;
        else if (lmnem == "xor")  arith = OperationType::bit_xor;
        // x86 shifts
        else if (lmnem == "shl" || lmnem == "sal") arith = OperationType::shl;
        else if (lmnem == "shr")  arith = OperationType::shr_us;
        else if (lmnem == "sar")  arith = OperationType::shr;
        // x86 rotates
        else if (lmnem == "rol")  arith = OperationType::left_rotate;
        else if (lmnem == "ror")  arith = OperationType::right_rotate;
        else if (lmnem == "rcl")  arith = OperationType::left_rotate_carry;
        else if (lmnem == "rcr")  arith = OperationType::right_rotate_carry;
        // ARM arithmetic
        else if (lmnem == "adds") arith = OperationType::add;
        else if (lmnem == "subs") arith = OperationType::sub;
        else if (lmnem == "mul")  arith = OperationType::mul;
        else if (lmnem == "sdiv") arith = OperationType::div;
        else if (lmnem == "udiv") arith = OperationType::div_us;
        // ARM logic
        else if (lmnem == "ands" || lmnem == "tst") arith = OperationType::bit_and;
        else if (lmnem == "orr")  arith = OperationType::bit_or;
        else if (lmnem == "eor")  arith = OperationType::bit_xor;
        // ARM shifts
        else if (lmnem == "lsl")  arith = OperationType::shl;
        else if (lmnem == "lsr")  arith = OperationType::shr_us;
        else if (lmnem == "asr")  arith = OperationType::shr;
        else if (lmnem == "ror" /* ARM ror */) arith = OperationType::right_rotate;
        // ARM multiply-add/sub
        else if (lmnem == "madd" && operands.size() >= 4) {
            // MADD Xd, Xn, Xm, Xa -> Xd = Xa + Xn * Xm
            auto* product = arena_.create<Operation>(OperationType::mul,
                std::vector<Expression*>{operands[1], operands[2]}, operands[0]->size_bytes);
            auto* sum = arena_.create<Operation>(OperationType::add,
                std::vector<Expression*>{operands[3], product}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], sum);
            assign->set_address(addr);
            return assign;
        }
        else if (lmnem == "msub" && operands.size() >= 4) {
            // MSUB Xd, Xn, Xm, Xa -> Xd = Xa - Xn * Xm
            auto* product = arena_.create<Operation>(OperationType::mul,
                std::vector<Expression*>{operands[1], operands[2]}, operands[0]->size_bytes);
            auto* diff = arena_.create<Operation>(OperationType::sub,
                std::vector<Expression*>{operands[3], product}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], diff);
            assign->set_address(addr);
            return assign;
        }

        if (arith != OperationType::unknown) {
            auto* result = make_binary_assign(arith, operands, addr);
            if (result) return result;
        }
    }

    // =====================================================================
    // Unary operations: NOT, NEG, INC, DEC
    // =====================================================================
    if (lmnem == "not" && !operands.empty()) {
        return make_unary_assign(OperationType::bit_not, operands, addr);
    }
    if (lmnem == "neg" && !operands.empty()) {
        return make_unary_assign(OperationType::negate, operands, addr);
    }
    if ((lmnem == "inc") && !operands.empty()) {
        auto* one = arena_.create<Constant>(1, operands[0]->size_bytes);
        auto* rhs = arena_.create<Operation>(OperationType::add,
            std::vector<Expression*>{operands[0], one}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], rhs);
        assign->set_address(addr);
        return assign;
    }
    if ((lmnem == "dec") && !operands.empty()) {
        auto* one = arena_.create<Constant>(1, operands[0]->size_bytes);
        auto* rhs = arena_.create<Operation>(OperationType::sub,
            std::vector<Expression*>{operands[0], one}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], rhs);
        assign->set_address(addr);
        return assign;
    }
    // ARM MVN (bitwise NOT)
    if (lmnem == "mvn" && operands.size() >= 2) {
        auto* rhs = arena_.create<Operation>(OperationType::bit_not,
            std::vector<Expression*>{operands[1]}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], rhs);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // LEA -- load effective address (x86)
    // =====================================================================
    if (lmnem == "lea" && operands.size() >= 2) {
        // LEA dest, [address_expression]
        // The memory operand was lifted as deref(addr), strip the deref
        Expression* effective_addr = operands[1];
        if (auto* deref_op = dyn_cast<Operation>(effective_addr)) {
            if (deref_op->type() == OperationType::deref && !deref_op->operands().empty()) {
                effective_addr = deref_op->operands()[0];
            }
        }
        auto* assign = arena_.create<Assignment>(operands[0], effective_addr);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // Data movement: MOV, MOVSX, MOVZX, LDR, STR, etc.
    // =====================================================================
    {
        bool is_mov = (lmnem == "mov" || lmnem == "movabs");
        bool is_load = (lmnem == "ldr" || lmnem == "ldur" || lmnem == "ldrb" ||
                        lmnem == "ldrh" || lmnem == "ldrsb" || lmnem == "ldrsh" ||
                        lmnem == "ldrsw");
        bool is_store = (lmnem == "str" || lmnem == "stur" || lmnem == "strb" ||
                         lmnem == "strh");
        bool is_movsx = (lmnem == "movsx" || lmnem == "movsxd" || lmnem == "cwde" ||
                         lmnem == "cdqe" || lmnem == "cbw");
        bool is_movzx = (lmnem == "movzx");
        bool is_xchg = (lmnem == "xchg");

        if (is_movsx && operands.size() >= 2) {
            // Sign-extend: cast src to dest type
            auto* cast_op = arena_.create<Operation>(OperationType::cast,
                std::vector<Expression*>{operands[1]}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], cast_op);
            assign->set_address(addr);
            return assign;
        }
        if (is_movzx && operands.size() >= 2) {
            // Zero-extend: cast src to dest type
            auto* cast_op = arena_.create<Operation>(OperationType::cast,
                std::vector<Expression*>{operands[1]}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], cast_op);
            assign->set_address(addr);
            return assign;
        }
        if (is_xchg && operands.size() >= 2) {
            // XCHG: swap two operands. Emit as tmp = a; a = b; b = tmp;
            // For simplicity, just emit as two assignments (not perfectly atomic)
            auto* tmp = arena_.create<Variable>("xchg_tmp", operands[0]->size_bytes);
            auto* a1 = arena_.create<Assignment>(tmp, operands[0]);
            a1->set_address(addr);
            // We can only return one instruction, so just return the first assignment
            // The second would require block-level splitting. For now, treat as mov.
            auto* assign = arena_.create<Assignment>(operands[0], operands[1]);
            assign->set_address(addr);
            return assign;
        }

        if ((is_mov || is_load || is_store) && operands.size() >= 2) {
            Expression* dest;
            Expression* src;
            if (is_store) {
                dest = operands[1];
                src = operands[0];
            } else {
                dest = operands[0];
                src = operands[1];
            }
            auto* assign = arena_.create<Assignment>(dest, src);
            assign->set_address(addr);
            return assign;
        }
    }

    // =====================================================================
    // ARM STP/LDP (store/load pair)
    // =====================================================================
    if (lmnem == "stp" && operands.size() >= 3) {
        // STP Xt1, Xt2, [addr] -- store pair. Approximate as store first reg.
        auto* assign = arena_.create<Assignment>(operands[2], operands[0]);
        assign->set_address(addr);
        return assign;
    }
    if (lmnem == "ldp" && operands.size() >= 3) {
        // LDP Xt1, Xt2, [addr] -- load pair. Approximate as load first reg.
        auto* assign = arena_.create<Assignment>(operands[0], operands[2]);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // ARM ADR/ADRP
    // =====================================================================
    if ((lmnem == "adr" || lmnem == "adrp") && operands.size() >= 2) {
        auto* assign = arena_.create<Assignment>(operands[0], operands[1]);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // ARM CSET / CSEL
    // =====================================================================
    if (lmnem == "cset" && operands.size() >= 2) {
        // CSET Xd, cond -> Xd = (cond ? 1 : 0)
        auto* one = arena_.create<Constant>(1, operands[0]->size_bytes);
        auto* zero = arena_.create<Constant>(0, operands[0]->size_bytes);
        auto* ternary = arena_.create<Operation>(OperationType::ternary,
            std::vector<Expression*>{operands[1], one, zero}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], ternary);
        assign->set_address(addr);
        return assign;
    }
    if (lmnem == "csel" && operands.size() >= 4) {
        // CSEL Xd, Xn, Xm, cond -> Xd = (cond ? Xn : Xm)
        auto* ternary = arena_.create<Operation>(OperationType::ternary,
            std::vector<Expression*>{operands[3], operands[1], operands[2]}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], ternary);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 CDQ/CWD/CQO -- sign-extend accumulator into edx:eax pair
    // =====================================================================
    if (lmnem == "cdq" || lmnem == "cwd" || lmnem == "cqo") {
        // These extend the sign of eax/ax/rax into edx/dx/rdx.
        // Approximate as: edx = sar(eax, 31)
        std::size_t sz = (lmnem == "cqo") ? 8 : (lmnem == "cdq") ? 4 : 2;
        std::string src_name = (lmnem == "cqo") ? "rax" : (lmnem == "cdq") ? "eax" : "ax";
        std::string dst_name = (lmnem == "cqo") ? "rdx" : (lmnem == "cdq") ? "edx" : "dx";
        auto* src = arena_.create<Variable>(src_name, sz);
        auto* shift_amt = arena_.create<Constant>(sz * 8 - 1, sz);
        auto* sar_op = arena_.create<Operation>(OperationType::shr,
            std::vector<Expression*>{src, shift_amt}, sz);
        auto* dst = arena_.create<Variable>(dst_name, sz);
        auto* assign = arena_.create<Assignment>(dst, sar_op);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 PUSH/POP -- stack operations (approximate)
    // =====================================================================
    if (lmnem == "push" && !operands.empty()) {
        // push src writes to [rsp - width] after stack-pointer decrement.
        // Prefer a named stack slot over raw dereference to reduce `*(rsp)` noise.
        const std::size_t push_width = operands[0]->size_bytes > 0
            ? operands[0]->size_bytes
            : static_cast<std::size_t>(8);
        auto* slot = resolve_sp_relative_slot(
            addr,
            -static_cast<std::int64_t>(push_width),
            push_width);
        auto* assign = arena_.create<Assignment>(slot, operands[0]);
        assign->set_address(addr);
        return assign;
    }
    if (lmnem == "pop" && !operands.empty()) {
        // pop dest reads from [rsp] before stack-pointer increment.
        const std::size_t pop_width = operands[0]->size_bytes > 0
            ? operands[0]->size_bytes
            : static_cast<std::size_t>(8);
        auto* slot = resolve_sp_relative_slot(addr, 0, pop_width);
        auto* assign = arena_.create<Assignment>(operands[0], slot);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 BSWAP
    // =====================================================================
    if (lmnem == "bswap" && !operands.empty()) {
        // Emit as a call to a __bswap intrinsic
        auto* target = arena_.create<Variable>("__bswap", 8);
        auto* call = arena_.create<Call>(target,
            std::vector<Expression*>{operands[0]}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], call);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 BT/BTS/BTR/BTC -- bit test operations
    // =====================================================================
    if ((lmnem == "bt" || lmnem == "bts" || lmnem == "btr" || lmnem == "btc")
        && operands.size() >= 2) {
        // BT base, offset: test bit. Sets CF. Approximate as bit_and + shift.
        auto* shifted = arena_.create<Operation>(OperationType::shr_us,
            std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
        auto* one = arena_.create<Constant>(1, operands[0]->size_bytes);
        auto* bit = arena_.create<Operation>(OperationType::bit_and,
            std::vector<Expression*>{shifted, one}, 1);
        auto* flags = arena_.create<Variable>("flags", 1);
        auto* assign = arena_.create<Assignment>(flags, bit);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 BSF/BSR -- bit scan
    // =====================================================================
    if ((lmnem == "bsf" || lmnem == "bsr") && operands.size() >= 2) {
        std::string intrinsic = (lmnem == "bsf") ? "__bsf" : "__bsr";
        auto* target = arena_.create<Variable>(intrinsic, 8);
        auto* call = arena_.create<Call>(target,
            std::vector<Expression*>{operands[1]}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], call);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 CMOVcc -- conditional moves
    // =====================================================================
    {
        OperationType cmov_cmp = OperationType::unknown;
        if      (lmnem == "cmove" || lmnem == "cmovz")       cmov_cmp = OperationType::eq;
        else if (lmnem == "cmovne" || lmnem == "cmovnz")     cmov_cmp = OperationType::neq;
        else if (lmnem == "cmovl" || lmnem == "cmovnge")     cmov_cmp = OperationType::lt;
        else if (lmnem == "cmovle" || lmnem == "cmovng")     cmov_cmp = OperationType::le;
        else if (lmnem == "cmovg" || lmnem == "cmovnle")     cmov_cmp = OperationType::gt;
        else if (lmnem == "cmovge" || lmnem == "cmovnl")     cmov_cmp = OperationType::ge;
        else if (lmnem == "cmovb" || lmnem == "cmovnae" || lmnem == "cmovc")  cmov_cmp = OperationType::lt_us;
        else if (lmnem == "cmovbe" || lmnem == "cmovna")     cmov_cmp = OperationType::le_us;
        else if (lmnem == "cmova" || lmnem == "cmovnbe")     cmov_cmp = OperationType::gt_us;
        else if (lmnem == "cmovae" || lmnem == "cmovnb" || lmnem == "cmovnc") cmov_cmp = OperationType::ge_us;
        else if (lmnem == "cmovs")  cmov_cmp = OperationType::lt;
        else if (lmnem == "cmovns") cmov_cmp = OperationType::ge;

        if (cmov_cmp != OperationType::unknown && operands.size() >= 2) {
            // CMOVcc dest, src -> dest = (flags cmp 0) ? src : dest
            auto* flags = arena_.create<Variable>("flags", 1);
            auto* zero = arena_.create<Constant>(0, 1);
            auto* cond = arena_.create<Condition>(cmov_cmp, flags, zero);
            auto* ternary = arena_.create<Operation>(OperationType::ternary,
                std::vector<Expression*>{cond, operands[1], operands[0]}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], ternary);
            assign->set_address(addr);
            return assign;
        }
    }

    // =====================================================================
    // x86 SETcc -- set byte on condition
    // =====================================================================
    {
        OperationType set_cmp = OperationType::unknown;
        if      (lmnem == "sete" || lmnem == "setz")       set_cmp = OperationType::eq;
        else if (lmnem == "setne" || lmnem == "setnz")     set_cmp = OperationType::neq;
        else if (lmnem == "setl" || lmnem == "setnge")     set_cmp = OperationType::lt;
        else if (lmnem == "setle" || lmnem == "setng")     set_cmp = OperationType::le;
        else if (lmnem == "setg" || lmnem == "setnle")     set_cmp = OperationType::gt;
        else if (lmnem == "setge" || lmnem == "setnl")     set_cmp = OperationType::ge;
        else if (lmnem == "setb" || lmnem == "setnae" || lmnem == "setc")  set_cmp = OperationType::lt_us;
        else if (lmnem == "setbe" || lmnem == "setna")     set_cmp = OperationType::le_us;
        else if (lmnem == "seta" || lmnem == "setnbe")     set_cmp = OperationType::gt_us;
        else if (lmnem == "setae" || lmnem == "setnb" || lmnem == "setnc") set_cmp = OperationType::ge_us;
        else if (lmnem == "sets")  set_cmp = OperationType::lt;
        else if (lmnem == "setns") set_cmp = OperationType::ge;

        if (set_cmp != OperationType::unknown && !operands.empty()) {
            // SETcc dest -> dest = (flags cmp 0) ? 1 : 0
            auto* flags = arena_.create<Variable>("flags", 1);
            auto* zero = arena_.create<Constant>(0, 1);
            auto* cond = arena_.create<Condition>(set_cmp, flags, zero);
            auto* one = arena_.create<Constant>(1, 1);
            auto* zero_val = arena_.create<Constant>(0, 1);
            auto* ternary = arena_.create<Operation>(OperationType::ternary,
                std::vector<Expression*>{cond, one, zero_val}, 1);
            auto* assign = arena_.create<Assignment>(operands[0], ternary);
            assign->set_address(addr);
            return assign;
        }
    }

    // =====================================================================
    // x86 DIV/IDIV -- implicit edx:eax operands
    // =====================================================================
    if ((lmnem == "div" || lmnem == "idiv") && !operands.empty()) {
        // DIV src: eax = edx:eax / src, edx = edx:eax % src
        // Approximate: eax = eax / src (ignoring edx high half)
        OperationType div_op = (lmnem == "div") ? OperationType::div_us : OperationType::div;
        std::size_t sz = operands[0]->size_bytes;
        std::string acc_name = (sz == 8) ? "rax" : (sz == 4) ? "eax" : (sz == 2) ? "ax" : "al";
        auto* acc = arena_.create<Variable>(acc_name, sz);
        auto* rhs = arena_.create<Operation>(div_op,
            std::vector<Expression*>{acc, operands[0]}, sz);
        auto* assign = arena_.create<Assignment>(acc, rhs);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 MUL -- unsigned multiply (implicit operands)
    // =====================================================================
    if (lmnem == "mul" && operands.size() == 1) {
        // MUL src: edx:eax = eax * src
        // Approximate: eax = eax * src
        std::size_t sz = operands[0]->size_bytes;
        std::string acc_name = (sz == 8) ? "rax" : (sz == 4) ? "eax" : (sz == 2) ? "ax" : "al";
        auto* acc = arena_.create<Variable>(acc_name, sz);
        auto* rhs = arena_.create<Operation>(OperationType::mul_us,
            std::vector<Expression*>{acc, operands[0]}, sz);
        auto* assign = arena_.create<Assignment>(acc, rhs);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // Fallback: wrap as unknown Operation inside an Assignment
    // =====================================================================
    auto* op = arena_.create<Operation>(OperationType::unknown, std::move(operands), insn.size());
    auto* mnem_var = arena_.create<Variable>(lmnem, insn.size());
    auto* assign = arena_.create<Assignment>(mnem_var, op);
    assign->set_address(addr);
    return assign;
}

} // namespace aletheia
