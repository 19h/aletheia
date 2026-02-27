#include "variable_name_generation.hpp"

#include <string>
#include <unordered_map>

namespace dewolf {

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

    if (dynamic_cast<const Pointer*>(type.get()) != nullptr) {
        return "p";
    }
    if (dynamic_cast<const Float*>(type.get()) != nullptr) {
        return "f";
    }
    if (dynamic_cast<const Integer*>(type.get()) != nullptr) {
        return "i";
    }
    if (type->is_boolean()) {
        return "b";
    }
    return "v";
}

std::string allocate_name_for_variable(const Variable* var, RenameState& state) {
    if (state.scheme == RenameState::Scheme::Default) {
        return "var_" + std::to_string(state.next_id++);
    }

    const std::string prefix = type_prefix_for(var ? var->ir_type() : nullptr);
    const std::size_t id = state.prefix_next_id[prefix]++;
    return prefix + "Var" + std::to_string(id);
}

void rename_variable(Variable* var, RenameState& state) {
    if (!var) return;

    if (auto it = state.pointer_name.find(var); it != state.pointer_name.end()) {
        var->set_name(it->second);
        var->set_ssa_version(0);
        return;
    }

    const VarKey key{var->name(), var->ssa_version()};
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

    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
        rename_expression(assign->destination(), state);
        rename_expression(assign->value(), state);
        return;
    }

    if (auto* branch = dynamic_cast<Branch*>(inst)) {
        rename_expression(branch->condition(), state);
        return;
    }

    if (auto* ib = dynamic_cast<IndirectBranch*>(inst)) {
        rename_expression(ib->expression(), state);
        return;
    }

    if (auto* ret = dynamic_cast<Return*>(inst)) {
        for (Expression* value : ret->values()) {
            rename_expression(value, state);
        }
        return;
    }

    if (auto* relation = dynamic_cast<Relation*>(inst)) {
        rename_variable(relation->destination(), state);
        rename_variable(relation->value(), state);
    }
}

void rename_expression(Expression* expr, RenameState& state) {
    if (!expr) return;

    if (auto* var = dynamic_cast<Variable*>(expr)) {
        rename_variable(var, state);
        return;
    }

    if (auto* op = dynamic_cast<Operation*>(expr)) {
        for (Expression* child : op->operands()) {
            rename_expression(child, state);
        }
        return;
    }

    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression* child : list->operands()) {
            rename_expression(child, state);
        }
    }
}

void rename_ast_node(AstNode* node, RenameState& state) {
    if (!node) return;

    if (auto* code = dynamic_cast<CodeNode*>(node)) {
        if (!code->block()) return;
        for (Instruction* inst : code->block()->instructions()) {
            rename_instruction(inst, state);
        }
        return;
    }

    if (auto* expr_node = dynamic_cast<ExprAstNode*>(node)) {
        rename_expression(expr_node->expr(), state);
        return;
    }

    if (auto* seq = dynamic_cast<SeqNode*>(node)) {
        for (AstNode* child : seq->nodes()) {
            rename_ast_node(child, state);
        }
        return;
    }

    if (auto* if_node = dynamic_cast<IfNode*>(node)) {
        rename_ast_node(if_node->cond(), state);
        rename_ast_node(if_node->true_branch(), state);
        rename_ast_node(if_node->false_branch(), state);
        return;
    }

    if (auto* loop = dynamic_cast<LoopNode*>(node)) {
        rename_expression(loop->condition(), state);
        rename_ast_node(loop->body(), state);

        if (auto* for_loop = dynamic_cast<ForLoopNode*>(loop)) {
            rename_instruction(for_loop->declaration(), state);
            rename_instruction(for_loop->modification(), state);
        }
        return;
    }

    if (auto* sw = dynamic_cast<SwitchNode*>(node)) {
        rename_ast_node(sw->cond(), state);
        for (CaseNode* c : sw->cases()) {
            rename_ast_node(c, state);
        }
        return;
    }

    if (auto* c = dynamic_cast<CaseNode*>(node)) {
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

} // namespace dewolf
