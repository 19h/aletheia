#include "variable_name_generation.hpp"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aletheia {

namespace {

struct VarKey {
    std::string name;
    std::size_t version = 0;

    bool operator==(const VarKey& other) const {
        return name == other.name && version == other.version;
    }
};

struct VarKeyHash {
    std::size_t operator()(const VarKey& key) const {
        return std::hash<std::string>{}(key.name) ^ (std::hash<std::size_t>{}(key.version) << 1U);
    }
};

struct RenameState {
    std::unordered_map<VarKey, std::string, VarKeyHash> canonical_name;
    std::unordered_map<Variable*, std::string> pointer_name;
    std::unordered_map<std::string, std::size_t> prefix_next_id;
    std::size_t next_id = 0;
    enum class Scheme {
        Default,
        SystemHungarian,
    } scheme = Scheme::Default;
};

void rename_expression(Expression* expr, RenameState& state);

std::string type_prefix_for(const TypePtr& type) {
    if (!type) {
        return "v";
    }

    if (type_isa<Pointer>(type.get())) {
        return "p";
    }
    if (type_isa<Float>(type.get())) {
        return "f";
    }
    if (type_isa<Integer>(type.get())) {
        return "i";
    }
    if (type->is_boolean()) {
        return "b";
    }
    return "v";
}

bool is_all_digits(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

std::optional<int> infer_parameter_index_from_register_name(std::string_view name) {
    if ((name.starts_with("x") || name.starts_with("w")) && is_all_digits(name.substr(1))) {
        return std::stoi(std::string(name.substr(1)));
    }

    // System V AMD64 ABI: rdi, rsi, rdx, rcx, r8, r9
    if (name == "rdi" || name == "edi") return 0;
    if (name == "rsi" || name == "esi") return 1;
    if (name == "rdx" || name == "edx") return 2;
    if (name == "rcx" || name == "ecx") return 3;
    if (name == "r8" || name == "r8d" || name == "r8w" || name == "r8b") return 4;
    if (name == "r9" || name == "r9d" || name == "r9w" || name == "r9b") return 5;

    return std::nullopt;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool has_generated_name_prefix(const std::string& name) {
    return name.starts_with("var_")
        || name.starts_with("tmp_")
        || name.starts_with("arg_")
        || name.starts_with("local_")
        || name.starts_with("sp_local_")
        || name.starts_with("sp_arg_")
        || name.starts_with("entry_")
        || name.starts_with("exit_");
}

bool is_register_like_name(std::string_view name) {
    if (name.empty()) return false;
    
    if (name.size() >= 2) {
        if (name[0] == 'r') {
            if (name[1] == 'a' || name[1] == 'b' || name[1] == 'c' || name[1] == 'd' || name[1] == 's' || name[1] == 'i' || name[1] == 'd') {
                return name.size() == 3 || (name.size() > 3 && std::isdigit(name[2]));
            }
            if (std::isdigit(name[1])) {
                return true;
            }
            if (name[1] == '8' || name[1] == '9') return name.size() == 3;
        }
        if (name[0] == 'e' && std::isdigit(name[1])) {
            return true;
        }
        if ((name[0] == 'x' || name[0] == 'w') && name.size() >= 2) {
            if (std::isdigit(name[1])) {
                return true;
            }
            if (name.size() >= 3 && name[1] == 's' && std::isdigit(name[2])) {
                return true;
            }
        }
        if (name.size() >= 2) {
            if (name[0] == 's' && (name == "st0" || name == "st1" || name == "st2" || name == "st3" ||
                                    name == "st4" || name == "st5" || name == "st6" || name == "st7")) {
                return true;
            }
            if ((name[0] == 'm' || name[0] == 'x' || name[0] == 'y' || name[0] == 'z') && name.size() >= 3) {
                if (name.substr(0, 2) == "mm" || name.substr(0, 3) == "xmm" || name.substr(0, 3) == "ymm" || name.substr(0, 3) == "zmm") {
                    if (name.size() >= 4 && std::isdigit(name[name.size() - 1])) {
                        return true;
                    }
                }
            }
        }
    }
    
    if (name.size() == 2) {
        if (name[0] == 'a' && (name[1] == 'h' || name[1] == 'l')) return true;
        if (name[0] == 'b' && (name[1] == 'h' || name[1] == 'l')) return true;
        if (name[0] == 'c' && (name[1] == 'h' || name[1] == 'l')) return true;
        if (name[0] == 'd' && (name[1] == 'h' || name[1] == 'l')) return true;
        if (name == "si" || name == "di" || name == "bp" || name == "sp") return true;
    }
    if (name.size() == 3) {
        if (name == "eax" || name == "ebx" || name == "ecx" || name == "edx" ||
            name == "esi" || name == "edi" || name == "ebp" || name == "esp" ||
            name == "rax" || name == "rbx" || name == "rcx" || name == "rdx" ||
            name == "rsi" || name == "rdi" || name == "rbp" || name == "rsp" ||
            name == "r8d" || name == "r9d" || name == "r8w" || name == "r9w" ||
            name == "r8b" || name == "r9b" || name == "sil" || name == "dil") return true;
        if (name == "r8" || name == "r9" || name == "r10" || name == "r11" ||
            name == "r12" || name == "r13" || name == "r14" || name == "r15") return true;
    }
    // Extended x86-64 register variants (r10d-r15d, r10w-r15w, r10b-r15b)
    if (name.size() == 4) {
        if ((name.starts_with("r1") && std::isdigit(name[2])) &&
            (name[3] == 'd' || name[3] == 'w' || name[3] == 'b')) return true;
    }
    
    if (name == "rip") return true;
    if (name == "sp" || name == "fp" || name == "pc" || name == "lr" || name == "cpsr") return true;
    if (name == "cs" || name == "ds" || name == "es" || name == "fs" || name == "gs" || name == "ss") return true;
    if (name == "eflags" || name == "rflags" || name == "flags") return true;
    
    return false;
}

std::string make_stack_name(const std::string& prefix, std::int64_t offset) {
    if (offset < 0) {
        return prefix + "_m" + std::to_string(-(offset + 1) + 1);
    }
    return prefix + "_" + std::to_string(offset);
}

VarKey canonical_key_for(const Variable* var) {
    if (!var) {
        return {"<null>", 0};
    }

    if (var->is_parameter()) {
        int index = var->parameter_index();
        if (index < 0) {
            auto inferred = infer_parameter_index_from_register_name(lower_ascii(var->name()));
            if (inferred.has_value()) {
                index = *inferred;
            }
        }
        if (index >= 0) {
            return {"param_" + std::to_string(index), 0};
        }
    }

    if (var->kind() == VariableKind::StackLocal || var->kind() == VariableKind::StackArgument) {
        return {"stack_" + std::to_string(var->stack_offset()), 0};
    }

    return {var->name(), var->ssa_version()};
}

std::string allocate_name_for_variable(const Variable* var, RenameState& state) {
    if (state.scheme == RenameState::Scheme::Default) {
        if (!var) {
            return "var_" + std::to_string(state.next_id++);
        }

        if (var->is_parameter()) {
            int index = var->parameter_index();
            if (index < 0) {
                auto inferred = infer_parameter_index_from_register_name(lower_ascii(var->name()));
                if (inferred.has_value()) {
                    index = *inferred;
                }
            }
            if (index >= 0) {
                return "arg_" + std::to_string(index);
            }
            return "arg_" + std::to_string(state.prefix_next_id["arg"]++);
        }

        if (var->kind() == VariableKind::StackArgument) {
            if (var->stack_offset() > 0) {
                return make_stack_name("arg", var->stack_offset());
            }
            return "arg_" + std::to_string(state.prefix_next_id["arg"]++);
        }

        if (var->kind() == VariableKind::StackLocal) {
            if (var->stack_offset() != 0) {
                return make_stack_name("local", var->stack_offset());
            }
            return "local_" + std::to_string(state.prefix_next_id["local"]++);
        }

        if (var->kind() == VariableKind::Temporary) {
            if (var->ir_type() && var->ir_type()->is_boolean()) {
                return "flag_" + std::to_string(state.prefix_next_id["flag"]++);
            }
            return "tmp_" + std::to_string(state.prefix_next_id["tmp"]++);
        }

        std::string lower = lower_ascii(var->name());
        if (lower.starts_with("entry_")) {
            return "entry_flag_" + std::to_string(state.prefix_next_id["entry_flag"]++);
        }
        if (lower.starts_with("exit_")) {
            return "exit_flag_" + std::to_string(state.prefix_next_id["exit_flag"]++);
        }

        if (lower.starts_with("local_") || lower.starts_with("arg_")
            || lower.starts_with("sp_local_") || lower.starts_with("sp_arg_")
            || lower.starts_with("mem_") || lower.starts_with("xmm") || lower.starts_with("ymm")) {
            return "tmp_" + std::to_string(state.prefix_next_id["tmp"]++);
        }

        if (auto inferred = infer_parameter_index_from_register_name(lower); inferred.has_value()) {
            return "arg_" + std::to_string(*inferred);
        }

        if (!has_generated_name_prefix(lower) && !lower.empty()) {
            if (is_register_like_name(lower)) {
                return "tmp_" + std::to_string(state.prefix_next_id["tmp"]++);
            }
            return var->name();
        }

        return "tmp_" + std::to_string(state.prefix_next_id["tmp"]++);
    }

    const std::string prefix = type_prefix_for(var ? var->ir_type() : nullptr);
    const std::size_t id = state.prefix_next_id[prefix]++;
    return prefix + "Var" + std::to_string(id);
}

void rename_variable(Variable* var, RenameState& state) {
    if (!var) return;
    if (isa<GlobalVariable>(var)) return;

    if (auto it = state.pointer_name.find(var); it != state.pointer_name.end()) {
        var->set_name(it->second);
        var->set_ssa_version(0);
        return;
    }

    const VarKey key = canonical_key_for(var);
    auto it = state.canonical_name.find(key);
    if (it == state.canonical_name.end()) {
        const std::string assigned = allocate_name_for_variable(var, state);
        it = state.canonical_name.emplace(key, assigned).first;
    }

    state.pointer_name.emplace(var, it->second);
    var->set_name(it->second);
    var->set_ssa_version(0);
}

void rename_instruction(Instruction* inst, RenameState& state) {
    if (!inst) return;

    if (auto* assign = dyn_cast<Assignment>(inst)) {
        rename_expression(assign->destination(), state);
        rename_expression(assign->value(), state);
        return;
    }

    if (auto* branch = dyn_cast<Branch>(inst)) {
        rename_expression(branch->condition(), state);
        return;
    }

    if (auto* ib = dyn_cast<IndirectBranch>(inst)) {
        rename_expression(ib->expression(), state);
        return;
    }

    if (auto* ret = dyn_cast<Return>(inst)) {
        for (Expression* value : ret->values()) {
            rename_expression(value, state);
        }
        return;
    }

    if (auto* relation = dyn_cast<Relation>(inst)) {
        rename_variable(relation->destination(), state);
        rename_variable(relation->value(), state);
    }
}

void rename_expression(Expression* expr, RenameState& state) {
    if (!expr) return;

    if (auto* var = dyn_cast<Variable>(expr)) {
        rename_variable(var, state);
        return;
    }

    if (auto* op = dyn_cast<Operation>(expr)) {
        // For Call nodes, skip renaming the target (operand 0) if it is
        // a resolved function name (GlobalVariable) — rename only the args.
        if (auto* call = dyn_cast<Call>(op)) {
            for (std::size_t i = 0; i < call->arg_count(); ++i) {
                rename_expression(call->arg(i), state);
            }
            return;
        }
        for (Expression* child : op->operands()) {
            rename_expression(child, state);
        }
        return;
    }

    if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            rename_expression(child, state);
        }
    }
}

void rename_ast_node(AstNode* node, RenameState& state) {
    if (!node) return;

    if (auto* code = ast_dyn_cast<CodeNode>(node)) {
        if (!code->block()) return;
        for (Instruction* inst : code->block()->instructions()) {
            rename_instruction(inst, state);
        }
        return;
    }

    if (auto* expr_node = ast_dyn_cast<ExprAstNode>(node)) {
        rename_expression(expr_node->expr(), state);
        return;
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode* child : seq->nodes()) {
            rename_ast_node(child, state);
        }
        return;
    }

    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        rename_ast_node(if_node->cond(), state);
        rename_ast_node(if_node->true_branch(), state);
        rename_ast_node(if_node->false_branch(), state);
        return;
    }

    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        rename_expression(loop->condition(), state);
        rename_ast_node(loop->body(), state);

        if (auto* for_loop = ast_dyn_cast<ForLoopNode>(loop)) {
            rename_instruction(for_loop->declaration(), state);
            rename_instruction(for_loop->modification(), state);
        }
        return;
    }

    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        rename_ast_node(sw->cond(), state);
        for (CaseNode* c : sw->cases()) {
            rename_ast_node(c, state);
        }
        return;
    }

    if (auto* c = ast_dyn_cast<CaseNode>(node)) {
        rename_ast_node(c->body(), state);
    }
}

} // namespace

void VariableNameGeneration::apply_default(AbstractSyntaxForest* forest) {
    if (!forest || !forest->root()) {
        return;
    }

    RenameState state;
    state.scheme = RenameState::Scheme::Default;
    rename_ast_node(forest->root(), state);
}

void VariableNameGeneration::apply_system_hungarian(AbstractSyntaxForest* forest) {
    if (!forest || !forest->root()) {
        return;
    }

    RenameState state;
    state.scheme = RenameState::Scheme::SystemHungarian;
    rename_ast_node(forest->root(), state);
}

void VariableNameGeneration::apply_to_cfg(ControlFlowGraph* cfg) {
    if (!cfg) {
        return;
    }

    RenameState state;
    state.scheme = RenameState::Scheme::Default;

    for (BasicBlock* block : cfg->blocks()) {
        if (!block) continue;
        for (Instruction* inst : block->instructions()) {
            rename_instruction(inst, state);
        }
    }
}

} // namespace aletheia
