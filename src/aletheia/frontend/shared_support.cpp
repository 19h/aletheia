#include "shared_support.hpp"

#include <ida/database.hpp>
#include <ida/function.hpp>
#include <ida/type.hpp>
#include <algorithm>
#include <cctype>

namespace aletheia::frontend {

namespace {

constexpr std::string_view kX86_64SysVIntRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
constexpr std::string_view kArm64IntRegs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};

} // namespace

std::string strip_ida_address_prefix(const std::string& name) {
    if (name.size() < 6) return name;
    if (name[0] != '_' || name[1] != '_') return name;
    if (name[name.size() - 1] != '_' || name[name.size() - 2] != '_') return name;

    std::size_t i = 2;
    while (i < name.size() && std::isxdigit(static_cast<unsigned char>(name[i]))) {
        ++i;
    }

    const std::size_t hex_count = i - 2;
    if (hex_count >= 8 && i < name.size() - 2) {
        std::string inner = name.substr(i, name.size() - i - 2);
        if (!inner.empty()) {
            return inner;
        }
    }
    return name;
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
    if (std::isdigit(static_cast<unsigned char>(text.front()))) {
        text = "g_" + text;
    }
    return strip_ida_address_prefix(text);
}

std::string to_lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string detect_arch() {
    auto proc = ida::database::processor_name();
    if (!proc) {
        return {};
    }
    std::string p = to_lower_ascii(*proc);
    if (p.find("arm") != std::string::npos || p.find("aarch64") != std::string::npos) {
        return "arm64";
    }
    if (p.find("metapc") != std::string::npos || p.find("x86") != std::string::npos || p.find("pc") != std::string::npos) {
        return "x86_64";
    }
    return {};
}

std::string canonical_function_name(std::string name) {
    name = to_lower_ascii(std::move(name));
    while (!name.empty() && name.front() == '_') {
        name.erase(name.begin());
    }
    return name;
}

std::vector<std::string> x86_64_sub_register_aliases(std::string_view reg64) {
    struct NamedAlias {
        std::string_view r64;
        std::string_view r32;
        std::string_view r16;
        std::string_view r8;
    };

    static constexpr NamedAlias kNamedAliases[] = {
        {"rax", "eax", "ax", "al"},
        {"rbx", "ebx", "bx", "bl"},
        {"rcx", "ecx", "cx", "cl"},
        {"rdx", "edx", "dx", "dl"},
        {"rsi", "esi", "si", "sil"},
        {"rdi", "edi", "di", "dil"},
        {"rbp", "ebp", "bp", "bpl"},
        {"rsp", "esp", "sp", "spl"},
    };

    for (const auto& alias : kNamedAliases) {
        if (reg64 == alias.r64) {
            return {std::string(alias.r32), std::string(alias.r16), std::string(alias.r8)};
        }
    }

    if (reg64.size() >= 2 && reg64[0] == 'r' && std::isdigit(static_cast<unsigned char>(reg64[1]))) {
        std::string base(reg64);
        return {base + "d", base + "w", base + "b"};
    }
    return {};
}

std::pair<const std::string_view*, std::size_t> param_register_table_for_arch(std::string_view arch) {
    if (arch == "arm64") {
        return {kArm64IntRegs, std::size(kArm64IntRegs)};
    }
    if (arch == "x86_64") {
        return {kX86_64SysVIntRegs, std::size(kX86_64SysVIntRegs)};
    }
    return {nullptr, 0};
}

bool first_parameter_prefers_w_reg(const TypePtr& function_type) {
    auto* fn = type_dyn_cast<FunctionTypeDef>(function_type.get());
    if (!fn || fn->parameters().empty()) {
        return false;
    }
    auto* p0 = type_dyn_cast<Integer>(fn->parameters()[0].get());
    return p0 && p0->size_bytes() <= 4;
}

bool return_prefers_w_reg(const TypePtr& function_type) {
    auto* fn = type_dyn_cast<FunctionTypeDef>(function_type.get());
    if (!fn || !fn->return_type()) {
        return false;
    }
    auto* ret = type_dyn_cast<Integer>(fn->return_type().get());
    return ret && ret->size_bytes() <= 4;
}

std::optional<std::size_t> known_call_min_arity(const std::string& canon_name) {
    if (canon_name == "error") return 0;
    if (canon_name == "clock_gettime") return 2;
    if (canon_name == "bzero") return 2;
    if (canon_name == "strtol") return 3;
    if (canon_name == "puts") return 1;
    if (canon_name == "putchar") return 1;
    if (canon_name == "printf") return 1;
    if (canon_name == "fprintf") return 2;
    return std::nullopt;
}

std::optional<bool> known_call_arg_prefers_w(const std::string& canon_name, std::size_t arg_index) {
    if (canon_name == "strtol" && arg_index == 2) {
        return true;
    }
    return std::nullopt;
}

TypePtr known_call_return_type(const std::string& canon_name) {
    if (canon_name == "error") {
        return std::make_shared<Pointer>(Integer::int32_t());
    }
    return nullptr;
}

SignatureState populate_task_signature(DecompilerTask& task) {
    SignatureState state;
    const ida::Address ea = task.function_address();

    std::string name = "sub_" + std::to_string(ea);
    if (auto name_res = ida::function::name_at(ea)) {
        name = *name_res;
    }
    task.set_function_name(name);
    task.clear_parameter_registers();

    const std::string arch = detect_arch();
    std::size_t param_count = 0;
    bool function_prototype_known = false;

    if (auto type_res = ida::type::retrieve(ea)) {
        auto& type_info = *type_res;
        TypeParser parser;

        if (type_info.is_function()) {
            function_prototype_known = true;
            auto ret_res = type_info.function_return_type();
            auto args_res = type_info.function_argument_types();

            TypePtr ret_type = CustomType::void_type();
            if (ret_res) {
                auto ret_str = ret_res->to_string();
                if (ret_str) {
                    ret_type = parser.parse(*ret_str);
                }
            }

            std::vector<TypePtr> params;
            if (args_res) {
                for (const auto& arg_type : *args_res) {
                    auto arg_str = arg_type.to_string();
                    if (arg_str) {
                        params.push_back(parser.parse(*arg_str));
                    }
                }
            }

            bool variadic = false;
            if (auto retrieved_variadic = type_info.is_variadic_function()) {
                variadic = *retrieved_variadic;
            }
            const std::string canonical_name = canonical_function_name(name);
            if (canonical_name == "printf" || canonical_name == "fprintf") {
                variadic = true;
            }

            task.set_function_type(std::make_shared<const FunctionTypeDef>(
                ret_type, params, variadic));
            param_count = params.size();
            state.current_function_param_count_hint = param_count;
            state.current_function_prefers_w_args = first_parameter_prefers_w_reg(task.function_type());
            state.current_function_prefers_w_return = return_prefers_w_reg(task.function_type());

            auto [reg_table, reg_count] = param_register_table_for_arch(arch);
            if (reg_table) {
                for (std::size_t i = 0; i < param_count && i < reg_count; ++i) {
                    const int idx = static_cast<int>(i);
                    const std::string display = "a" + std::to_string(i + 1);
                    const TypePtr ptype = i < params.size() ? params[i] : nullptr;

                    std::string reg_name(reg_table[i]);
                    if (arch == "arm64" && reg_name.size() >= 2 && reg_name[0] == 'x') {
                        if (auto* int_type = type_dyn_cast<Integer>(ptype.get())) {
                            if (int_type->size_bytes() <= 4) {
                                reg_name[0] = 'w';
                            }
                        }
                    }

                    state.param_register_map[reg_name] = idx;
                    task.set_parameter_register(reg_name, DecompilerTask::ParameterInfo{display, idx, ptype});

                    if (arch == "x86_64") {
                        for (const auto& alias : x86_64_sub_register_aliases(reg_name)) {
                            state.param_register_map[alias] = idx;
                            task.set_parameter_register(alias, DecompilerTask::ParameterInfo{display, idx, ptype});
                        }
                    }
                }
            }
        } else {
            auto type_str = type_info.to_string();
            if (type_str) {
                task.set_function_type(parser.parse(*type_str));
            }
        }
    }

    if (auto regvars = ida::function::register_variables(ea)) {
        for (const auto& rv : *regvars) {
            if (rv.user_name.empty()) {
                continue;
            }

            std::string canonical = to_lower_ascii(rv.canonical_name);
            std::string user_lower = to_lower_ascii(rv.user_name);
            state.regvar_alias_map[user_lower] = canonical;

            auto it = state.param_register_map.find(canonical);
            if (it != state.param_register_map.end()) {
                state.param_register_map[user_lower] = it->second;
                task.set_parameter_register(canonical,
                    DecompilerTask::ParameterInfo{rv.user_name, it->second, nullptr});
            }
        }
    }

    if (state.param_register_map.empty() && !function_prototype_known) {
        auto [reg_table, reg_count] = param_register_table_for_arch(arch);
        if (reg_table && reg_count > 0) {
            const std::size_t fallback_count = arch == "arm64"
                ? reg_count : std::min<std::size_t>(reg_count, 6);
            for (std::size_t i = 0; i < fallback_count; ++i) {
                std::string reg_name(reg_table[i]);
                const int idx = static_cast<int>(i);
                const std::string display = "arg_" + std::to_string(i);

                state.param_register_map[reg_name] = idx;
                task.set_parameter_register(reg_name, DecompilerTask::ParameterInfo{display, idx, nullptr});

                if (reg_name.size() >= 2 && reg_name[0] == 'x') {
                    std::string w_alias = reg_name;
                    w_alias[0] = 'w';
                    state.param_register_map[w_alias] = idx;
                    task.set_parameter_register(w_alias, DecompilerTask::ParameterInfo{display, idx, nullptr});
                }

                for (const auto& alias : x86_64_sub_register_aliases(reg_name)) {
                    state.param_register_map[alias] = idx;
                    task.set_parameter_register(alias, DecompilerTask::ParameterInfo{display, idx, nullptr});
                }
            }
        }
    }

    if (task.function_type()) {
        state.current_function_prefers_w_args = first_parameter_prefers_w_reg(task.function_type());
        state.current_function_prefers_w_return = return_prefers_w_reg(task.function_type());
    }

    return state;
}

FrameLayout build_frame_layout(ida::Address function_address) {
    FrameLayout frame_layout;
    auto frame_res = ida::function::frame(function_address);
    if (!frame_res) {
        return frame_layout;
    }

    const auto& sf = *frame_res;
    frame_layout.local_size = sf.local_variables_size();
    frame_layout.regs_size = sf.saved_registers_size();
    frame_layout.args_size = sf.arguments_size();
    frame_layout.total_size = sf.total_size();
    frame_layout.valid = true;

    const auto fp_base = static_cast<std::int64_t>(frame_layout.local_size + frame_layout.regs_size);
    for (const auto& fv : sf.variables()) {
        if (fv.is_special) {
            continue;
        }
        const auto fp_offset = static_cast<std::int64_t>(fv.byte_offset) - fp_base;
        frame_layout.offset_to_var[fp_offset] = fv;
    }

    return frame_layout;
}

std::optional<std::string> resolve_frame_variable(const FrameLayout& frame_layout,
                                                  std::int64_t frame_offset,
                                                  std::size_t) {
    if (!frame_layout.valid) {
        return std::nullopt;
    }

    auto it = frame_layout.offset_to_var.find(frame_offset);
    if (it != frame_layout.offset_to_var.end() && !it->second.name.empty()) {
        return it->second.name;
    }

    for (const auto& [off, fv] : frame_layout.offset_to_var) {
        if (fv.name.empty()) {
            continue;
        }
        if (frame_offset >= off && static_cast<std::size_t>(frame_offset - off) < fv.byte_size) {
            if (frame_offset == off) {
                return fv.name;
            }
            return fv.name + "_" + std::to_string(frame_offset - off);
        }
    }

    return std::nullopt;
}

Variable* resolve_sp_relative_slot(DecompilerArena& arena,
                                   const FrameLayout& frame_layout,
                                   ida::Address insn_addr,
                                   std::int64_t sp_adjust_bytes,
                                   std::size_t access_size) {
    const std::size_t width = access_size > 0 ? access_size : static_cast<std::size_t>(8);

    std::int64_t fp_offset = 0;
    bool have_fp_offset = false;

    if (frame_layout.valid) {
        if (auto sp_delta = ida::function::sp_delta_at(insn_addr)) {
            // IDA's SP delta is already measured from the function-entry SP.
            // Frame variables are indexed in the same signed coordinate after
            // build_frame_layout subtracts the FP base from member offsets.
            // Adding that base again shifts every local into the argument
            // region (e.g. -32 B + 32 B became arg_0 instead of local_-32).
            fp_offset = *sp_delta + sp_adjust_bytes;
            have_fp_offset = true;
        }
    }

    auto abs_i64 = [](std::int64_t value) -> std::uint64_t {
        return value < 0
            ? static_cast<std::uint64_t>(-(value + 1)) + 1ULL
            : static_cast<std::uint64_t>(value);
    };

    std::string slot_name;
    if (have_fp_offset) {
        if (auto resolved = resolve_frame_variable(frame_layout, fp_offset, width)) {
            slot_name = *resolved;
        }
    }

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
            if (auto sp_delta = ida::function::sp_delta_at(insn_addr)) {
                sp_key += *sp_delta;
            }
            const std::uint64_t abs_off = abs_i64(sp_key);
            slot_name = sp_key < 0 ? "sp_local_" + std::to_string(abs_off)
                                   : "sp_arg_" + std::to_string(abs_off);
        }
    }

    auto* slot = arena.create<Variable>(slot_name, width);
    slot->set_aliased(true);

    if (have_fp_offset) {
        slot->set_stack_offset(fp_offset);
        slot->set_kind(fp_offset >= 0 ? VariableKind::StackArgument : VariableKind::StackLocal);
        auto it = frame_layout.offset_to_var.find(fp_offset);
        if (it != frame_layout.offset_to_var.end() && it->second.byte_size > 0) {
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

} // namespace aletheia::frontend
