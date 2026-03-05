#include "variable_name_generation.hpp"

#include <cctype>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    std::unordered_map<int, VarKey> parameter_owner;
    std::unordered_set<VarKey, VarKeyHash> return_sink_keys;
    std::unordered_set<std::string> return_sink_names;
    std::size_t next_id = 0;
    enum class Scheme {
        Default,
        SystemHungarian,
    } scheme = Scheme::Default;
};

void rename_expression(Expression* expr, RenameState& state);

bool var_key_less(const VarKey& lhs, const VarKey& rhs) {
    if (lhs.name != rhs.name) {
        return lhs.name < rhs.name;
    }
    return lhs.version < rhs.version;
}

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
    // AArch64 AAPCS: x0-x7 / w0-w7 are parameter registers.
    if ((name.starts_with("x") || name.starts_with("w")) && is_all_digits(name.substr(1))) {
        int index = std::stoi(std::string(name.substr(1)));
        if (index <= 7) return index;
        return std::nullopt;  // x8+ / w8+ are not parameter registers
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

int effective_parameter_index(const Variable* var) {
    if (!var) {
        return -1;
    }
    int index = var->parameter_index();
    if (index < 0) {
        std::string lowered = var->name();
        for (char& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        auto inferred = infer_parameter_index_from_register_name(lowered);
        if (inferred.has_value()) {
            index = *inferred;
        }
    }
    return index;
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

    if (var->kind() == VariableKind::StackLocal || var->kind() == VariableKind::StackArgument) {
        return {"stack_" + std::to_string(var->stack_offset()), 0};
    }

    return {var->name(), var->ssa_version()};
}

bool is_return_sink_variable(const Variable* var, const RenameState& state) {
    if (!var) {
        return false;
    }
    const VarKey key = canonical_key_for(var);
    return state.return_sink_keys.contains(key)
        || state.return_sink_keys.contains(VarKey{key.name, 0})
        || state.return_sink_names.contains(key.name);
}

std::string allocate_name_for_variable(const Variable* var, RenameState& state) {
    if (state.scheme == RenameState::Scheme::Default) {
        if (!var) {
            return "var_" + std::to_string(state.next_id++);
        }

        if (var->is_parameter()) {
            int index = effective_parameter_index(var);
            if (index >= 0) {
                const VarKey owner_key{var->name(), var->ssa_version()};
                auto owner_it = state.parameter_owner.find(index);
                if (owner_it == state.parameter_owner.end()) {
                    state.parameter_owner.emplace(index, owner_key);
                    return "arg_" + std::to_string(index);
                }
                if (owner_it->second == owner_key) {
                    return "arg_" + std::to_string(index);
                }
                if (is_return_sink_variable(var, state)) {
                    return "ret_" + std::to_string(state.prefix_next_id["ret"]++);
                }
                // Multiple SSA aliases can legitimately represent the same
                // source-level parameter (e.g., AArch64 x0/w0 views).
                return "arg_" + std::to_string(index);
            }
            if (is_return_sink_variable(var, state)) {
                return "ret_" + std::to_string(state.prefix_next_id["ret"]++);
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

        const bool is_return_sink = is_return_sink_variable(var, state);
        if (is_return_sink
            && !var->is_parameter()
            && var->kind() != VariableKind::StackArgument
            && var->kind() != VariableKind::StackLocal) {
            return "ret_" + std::to_string(state.prefix_next_id["ret"]++);
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

void seed_parameter_owners_from_expression(Expression* expr, RenameState& state) {
    if (!expr) {
        return;
    }

    if (auto* var = dyn_cast<Variable>(expr)) {
        if (var->is_parameter()) {
            const int index = effective_parameter_index(var);
            if (index >= 0) {
                const VarKey key{var->name(), var->ssa_version()};
                auto it = state.parameter_owner.find(index);
                if (it == state.parameter_owner.end() || var_key_less(key, it->second)) {
                    state.parameter_owner[index] = key;
                }
            }
        }
        return;
    }

    if (auto* op = dyn_cast<Operation>(expr)) {
        for (Expression* child : op->operands()) {
            seed_parameter_owners_from_expression(child, state);
        }
        return;
    }

    if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            seed_parameter_owners_from_expression(child, state);
        }
    }
}

void seed_parameter_owners_from_ast(AstNode* node, RenameState& state) {
    if (!node) {
        return;
    }

    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        seed_parameter_owners_from_ast(if_node->cond(), state);
        seed_parameter_owners_from_ast(if_node->true_branch(), state);
        seed_parameter_owners_from_ast(if_node->false_branch(), state);
        return;
    }

    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        seed_parameter_owners_from_expression(loop->condition(), state);
        if (auto* for_loop = ast_dyn_cast<ForLoopNode>(loop)) {
            if (auto* decl = for_loop->declaration()) {
                for (Variable* req : decl->requirements()) {
                    if (!req || !req->is_parameter()) continue;
                    const int index = effective_parameter_index(req);
                    if (index < 0) continue;
                    const VarKey key{req->name(), req->ssa_version()};
                    auto it = state.parameter_owner.find(index);
                    if (it == state.parameter_owner.end() || var_key_less(key, it->second)) {
                        state.parameter_owner[index] = key;
                    }
                }
            }
            if (auto* mod = for_loop->modification()) {
                for (Variable* req : mod->requirements()) {
                    if (!req || !req->is_parameter()) continue;
                    const int index = effective_parameter_index(req);
                    if (index < 0) continue;
                    const VarKey key{req->name(), req->ssa_version()};
                    auto it = state.parameter_owner.find(index);
                    if (it == state.parameter_owner.end() || var_key_less(key, it->second)) {
                        state.parameter_owner[index] = key;
                    }
                }
            }
        }
        seed_parameter_owners_from_ast(loop->body(), state);
        return;
    }

    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        seed_parameter_owners_from_ast(sw->cond(), state);
        for (CaseNode* c : sw->cases()) {
            seed_parameter_owners_from_ast(c, state);
        }
        return;
    }

    if (auto* expr_node = ast_dyn_cast<ExprAstNode>(node)) {
        seed_parameter_owners_from_expression(expr_node->expr(), state);
        return;
    }

    if (auto* code = ast_dyn_cast<CodeNode>(node)) {
        if (!code->block()) {
            return;
        }
        for (Instruction* inst : code->block()->instructions()) {
            if (auto* branch = dyn_cast<Branch>(inst)) {
                seed_parameter_owners_from_expression(branch->condition(), state);
            }
        }
        return;
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode* child : seq->nodes()) {
            seed_parameter_owners_from_ast(child, state);
        }
        return;
    }

    if (auto* c = ast_dyn_cast<CaseNode>(node)) {
        seed_parameter_owners_from_ast(c->body(), state);
    }
}

void collect_var_keys(Expression* expr, std::unordered_set<VarKey, VarKeyHash>& out) {
    if (!expr) {
        return;
    }
    if (auto* var = dyn_cast<Variable>(expr)) {
        const VarKey key = canonical_key_for(var);
        out.insert(key);
        if (key.version != 0) {
            out.insert(VarKey{key.name, 0});
        }
        return;
    }
    if (auto* op = dyn_cast<Operation>(expr)) {
        for (Expression* child : op->operands()) {
            collect_var_keys(child, out);
        }
        return;
    }
    if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            collect_var_keys(child, out);
        }
    }
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
        // For Call nodes, skip renaming the target (operand 0) only if it is
        // a resolved function name (GlobalVariable). Rename regular Variable
        // targets (indirect calls through registers) normally.
        if (auto* call = dyn_cast<Call>(op)) {
            if (!isa<GlobalVariable>(call->target())) {
                rename_expression(call->target(), state);
            }
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

void collect_return_sink_keys_from_ast(AstNode* node, std::unordered_set<VarKey, VarKeyHash>& out) {
    if (!node) {
        return;
    }

    if (auto* code = ast_dyn_cast<CodeNode>(node)) {
        if (!code->block()) {
            return;
        }
        for (Instruction* inst : code->block()->instructions()) {
            auto* ret = dyn_cast<Return>(inst);
            if (!ret) {
                continue;
            }
            for (Expression* value : ret->values()) {
                collect_var_keys(value, out);
            }
        }
        return;
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode* child : seq->nodes()) {
            collect_return_sink_keys_from_ast(child, out);
        }
        return;
    }

    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        collect_return_sink_keys_from_ast(if_node->cond(), out);
        collect_return_sink_keys_from_ast(if_node->true_branch(), out);
        collect_return_sink_keys_from_ast(if_node->false_branch(), out);
        return;
    }

    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        collect_return_sink_keys_from_ast(loop->body(), out);
        return;
    }

    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        collect_return_sink_keys_from_ast(sw->cond(), out);
        for (CaseNode* c : sw->cases()) {
            collect_return_sink_keys_from_ast(c, out);
        }
        return;
    }

    if (auto* c = ast_dyn_cast<CaseNode>(node)) {
        collect_return_sink_keys_from_ast(c->body(), out);
    }
}


} // namespace

void VariableNameGeneration::apply_default(AbstractSyntaxForest* forest) {
    if (!forest || !forest->root()) {
        return;
    }

    RenameState state;
    state.scheme = RenameState::Scheme::Default;
    collect_return_sink_keys_from_ast(forest->root(), state.return_sink_keys);
    for (const VarKey& key : state.return_sink_keys) {
        state.return_sink_names.insert(key.name);
    }
    seed_parameter_owners_from_ast(forest->root(), state);
    rename_ast_node(forest->root(), state);
}

void VariableNameGeneration::apply_system_hungarian(AbstractSyntaxForest* forest) {
    if (!forest || !forest->root()) {
        return;
    }

    RenameState state;
    state.scheme = RenameState::Scheme::SystemHungarian;
    collect_return_sink_keys_from_ast(forest->root(), state.return_sink_keys);
    for (const VarKey& key : state.return_sink_keys) {
        state.return_sink_names.insert(key.name);
    }
    seed_parameter_owners_from_ast(forest->root(), state);
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
            auto* ret = dyn_cast<Return>(inst);
            if (!ret) {
                continue;
            }
            for (Expression* value : ret->values()) {
                collect_var_keys(value, state.return_sink_keys);
            }
        }
    }
    for (const VarKey& key : state.return_sink_keys) {
        state.return_sink_names.insert(key.name);
    }

    for (BasicBlock* block : cfg->blocks()) {
        if (!block) continue;
        for (Instruction* inst : block->instructions()) {
            rename_instruction(inst, state);
        }
    }
}

namespace {

bool same_variable_identity(Variable* lhs, Variable* rhs) {
    if (!lhs || !rhs) return false;
    return lhs->name() == rhs->name() && lhs->ssa_version() == rhs->ssa_version();
}

bool contains_call_expression(Expression* expr) {
    if (!expr) return false;
    if (isa<Call>(expr)) return true;
    if (auto* op = dyn_cast<Operation>(expr)) {
        if (op->type() == OperationType::call) return true;
        for (Expression* child : op->operands()) {
            if (contains_call_expression(child)) return true;
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            if (contains_call_expression(child)) return true;
        }
    }
    return false;
}

std::size_t count_variable_uses(Expression* expr, Variable* target) {
    if (!expr || !target) {
        return 0;
    }
    if (auto* v = dyn_cast<Variable>(expr)) {
        return same_variable_identity(v, target) ? 1 : 0;
    }

    std::size_t count = 0;
    if (auto* op = dyn_cast<Operation>(expr)) {
        for (Expression* child : op->operands()) {
            count += count_variable_uses(child, target);
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            count += count_variable_uses(child, target);
        }
    }
    return count;
}

bool substitute_variable_once(Expression*& expr, Variable* target, Expression* replacement) {
    if (!expr || !target || !replacement) {
        return false;
    }
    if (expr == replacement) {
        return false;
    }
    if (auto* v = dyn_cast<Variable>(expr)) {
        if (same_variable_identity(v, target)) {
            expr = replacement;
            return true;
        }
        return false;
    }

    if (auto* op = dyn_cast<Operation>(expr)) {
        for (Expression*& child : op->mutable_operands()) {
            if (substitute_variable_once(child, target, replacement)) {
                return true;
            }
        }
        return false;
    }
    if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression*& child : list->mutable_operands()) {
            if (substitute_variable_once(child, target, replacement)) {
                return true;
            }
        }
        return false;
    }
    return false;
}

void remove_self_assignments_from_block(BasicBlock* block, DecompilerArena* arena) {
    if (!block) return;
    std::vector<Instruction*> filtered;
    const auto& insts = block->instructions();
    filtered.reserve(insts.size());

    for (std::size_t i = 0; i < insts.size(); ++i) {
        Instruction* inst = insts[i];

        // Fold terminal pattern:
        //   v = <expr>
        //   return v
        // into:
        //   return <expr>
        // This avoids disconnected return-via-parameter artifacts after
        // SSA rename/coalescing while preserving expression semantics.
        if (i + 1 < insts.size()) {
            auto* assign = dyn_cast<Assignment>(inst);
            auto* ret = dyn_cast<Return>(insts[i + 1]);
            auto* dst = assign ? dyn_cast<Variable>(assign->destination()) : nullptr;
            if (assign && ret && dst && ret->values().size() == 1) {
                const bool dst_in_value = count_variable_uses(assign->value(), dst) != 0;
                auto* ret_var = dyn_cast<Variable>(ret->values()[0]);
                if (same_variable_identity(dst, ret_var) && arena) {
                    ret->mutable_values()[0] = assign->value()->copy(*arena);
                    continue;
                }

                Expression*& ret_value = ret->mutable_values()[0];
                Expression* replacement = arena ? assign->value()->copy(*arena) : nullptr;
                if (!dst_in_value
                    && !contains_call_expression(assign->value())
                    && count_variable_uses(ret_value, dst) == 1
                    && replacement
                    && substitute_variable_once(ret_value, dst, replacement)) {
                    continue;
                }
            }
        }

        // Fold non-adjacent assignment -> return uses when safe:
        //   v = <expr>
        //   ... (no v use/redef)
        //   return <...v...>
        if (auto* assign = dyn_cast<Assignment>(inst)) {
            auto* dst = dyn_cast<Variable>(assign->destination());
            if (dst
                && !contains_call_expression(assign->value())
                && count_variable_uses(assign->value(), dst) == 0) {
                std::size_t ret_index = insts.size();
                bool blocked = false;
                bool used_before_return = false;

                for (std::size_t j = i + 1; j < insts.size(); ++j) {
                    Instruction* mid = insts[j];
                    if (!mid) {
                        continue;
                    }

                    for (Variable* def : mid->definitions()) {
                        auto* def_var = dyn_cast<Variable>(def);
                        if (same_variable_identity(dst, def_var)) {
                            blocked = true;
                            break;
                        }
                    }
                    if (blocked) {
                        break;
                    }

                    if (auto* ret = dyn_cast<Return>(mid)) {
                        if (ret->values().size() == 1) {
                            ret_index = j;
                        }
                        break;
                    }

                    for (Variable* req : mid->requirements()) {
                        auto* req_var = dyn_cast<Variable>(req);
                        if (same_variable_identity(dst, req_var)) {
                            used_before_return = true;
                        }
                    }
                }

                if (!blocked && ret_index < insts.size()) {
                    auto* ret = dyn_cast<Return>(insts[ret_index]);
                    Expression*& ret_value = ret->mutable_values()[0];
                    Expression* replacement = arena ? assign->value()->copy(*arena) : nullptr;
                    if (count_variable_uses(ret_value, dst) == 1
                        && replacement
                        && substitute_variable_once(ret_value, dst, replacement)) {
                        if (!used_before_return) {
                            continue;
                        }
                    }
                }
            }
        }

        if (auto* assign = dyn_cast<Assignment>(inst)) {
            auto* dst = dyn_cast<Variable>(assign->destination());
            auto* src = dyn_cast<Variable>(assign->value());
            if (same_variable_identity(dst, src)) {
                continue;  // skip self-assignment
            }
        }
        filtered.push_back(inst);
    }
    if (filtered.size() != block->instructions().size()) {
        block->set_instructions(std::move(filtered));
    }
}

void remove_self_assignments_from_ast(AstNode* node, DecompilerArena* arena) {
    if (!node) return;

    if (auto* code = ast_dyn_cast<CodeNode>(node)) {
        remove_self_assignments_from_block(code->block(), arena);
        return;
    }
    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode* child : seq->nodes()) {
            remove_self_assignments_from_ast(child, arena);
        }
        return;
    }
    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        remove_self_assignments_from_ast(if_node->cond(), arena);
        remove_self_assignments_from_ast(if_node->true_branch(), arena);
        remove_self_assignments_from_ast(if_node->false_branch(), arena);
        return;
    }
    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        remove_self_assignments_from_ast(loop->body(), arena);
        return;
    }
    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        remove_self_assignments_from_ast(sw->cond(), arena);
        for (CaseNode* c : sw->cases()) {
            remove_self_assignments_from_ast(c, arena);
        }
        return;
    }
    if (auto* c = ast_dyn_cast<CaseNode>(node)) {
        remove_self_assignments_from_ast(c->body(), arena);
    }
}

} // namespace (self-assignment helpers)

void VariableNameGeneration::remove_self_assignments(ControlFlowGraph* cfg, DecompilerArena* arena) {
    if (!cfg) return;
    for (BasicBlock* block : cfg->blocks()) {
        remove_self_assignments_from_block(block, arena);
    }
}

void VariableNameGeneration::remove_self_assignments_ast(AbstractSyntaxForest* forest, DecompilerArena* arena) {
    if (!forest || !forest->root()) return;
    remove_self_assignments_from_ast(forest->root(), arena);
}

} // namespace aletheia
