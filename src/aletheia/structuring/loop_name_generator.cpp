#include "loop_name_generator.hpp"

#include <array>
#include <string>
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

constexpr std::array<const char*, 12> kForNames = {
    "i", "j", "k", "m", "n", "p", "q", "r", "s", "t", "u", "v"
};

void rename_expression(Expression* expr, const std::unordered_map<VarKey, std::string, VarKeyHash>& names);

void rename_variable(Variable* var, const std::unordered_map<VarKey, std::string, VarKeyHash>& names) {
    if (!var) return;
    if (isa<GlobalVariable>(var)) return;
    VarKey key{var->name(), var->ssa_version()};
    auto it = names.find(key);
    if (it == names.end()) return;
    var->set_name(it->second);
    var->set_ssa_version(0);
}

void rename_instruction(Instruction* inst, const std::unordered_map<VarKey, std::string, VarKeyHash>& names) {
    if (!inst) return;

    if (auto* assign = dyn_cast<Assignment>(inst)) {
        rename_expression(assign->destination(), names);
        rename_expression(assign->value(), names);
        return;
    }
    if (auto* branch = dyn_cast<Branch>(inst)) {
        rename_expression(branch->condition(), names);
        return;
    }
    if (auto* ib = dyn_cast<IndirectBranch>(inst)) {
        rename_expression(ib->expression(), names);
        return;
    }
    if (auto* ret = dyn_cast<Return>(inst)) {
        for (Expression* value : ret->values()) {
            rename_expression(value, names);
        }
        return;
    }
    if (auto* rel = dyn_cast<Relation>(inst)) {
        rename_variable(rel->destination(), names);
        rename_variable(rel->value(), names);
    }
}

void rename_expression(Expression* expr, const std::unordered_map<VarKey, std::string, VarKeyHash>& names) {
    if (!expr) return;
    if (auto* var = dyn_cast<Variable>(expr)) {
        rename_variable(var, names);
        return;
    }
    if (auto* op = dyn_cast<Operation>(expr)) {
        for (Expression* child : op->operands()) {
            rename_expression(child, names);
        }
        return;
    }
    if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            rename_expression(child, names);
        }
    }
}

void collect_for_loop_names(
    AstNode* node,
    std::unordered_map<VarKey, std::string, VarKeyHash>& names,
    std::size_t& next_id) {
    if (!node) return;

    if (auto* seq = dynamic_cast<SeqNode*>(node)) {
        for (AstNode* child : seq->nodes()) {
            collect_for_loop_names(child, names, next_id);
        }
        return;
    }

    if (auto* if_node = dynamic_cast<IfNode*>(node)) {
        collect_for_loop_names(if_node->cond(), names, next_id);
        collect_for_loop_names(if_node->true_branch(), names, next_id);
        collect_for_loop_names(if_node->false_branch(), names, next_id);
        return;
    }

    if (auto* loop = dynamic_cast<LoopNode*>(node)) {
        if (auto* for_loop = dynamic_cast<ForLoopNode*>(loop)) {
            if (auto* decl = dyn_cast<Assignment>(for_loop->declaration())) {
                if (auto* dst = dyn_cast<Variable>(decl->destination())) {
                    VarKey key{dst->name(), dst->ssa_version()};
                    if (!names.contains(key)) {
                        if (next_id < kForNames.size()) {
                            names.emplace(std::move(key), kForNames[next_id]);
                        } else {
                            names.emplace(std::move(key), "i" + std::to_string(next_id));
                        }
                        next_id++;
                    }
                }
            }
        }

        collect_for_loop_names(loop->body(), names, next_id);
        return;
    }

    if (auto* sw = dynamic_cast<SwitchNode*>(node)) {
        collect_for_loop_names(sw->cond(), names, next_id);
        for (CaseNode* c : sw->cases()) {
            collect_for_loop_names(c, names, next_id);
        }
        return;
    }

    if (auto* c = dynamic_cast<CaseNode*>(node)) {
        collect_for_loop_names(c->body(), names, next_id);
    }
}

Variable* extract_condition_variable(Expression* expr) {
    auto* cond = dyn_cast<Condition>(expr);
    if (!cond) return nullptr;
    if (auto* lhs = dyn_cast<Variable>(cond->lhs())) {
        return lhs;
    }
    if (auto* rhs = dyn_cast<Variable>(cond->rhs())) {
        return rhs;
    }
    return nullptr;
}

void collect_while_loop_names(
    AstNode* node,
    std::unordered_map<VarKey, std::string, VarKeyHash>& names,
    std::size_t& next_id) {
    if (!node) return;

    if (auto* seq = dynamic_cast<SeqNode*>(node)) {
        for (AstNode* child : seq->nodes()) {
            collect_while_loop_names(child, names, next_id);
        }
        return;
    }

    if (auto* if_node = dynamic_cast<IfNode*>(node)) {
        collect_while_loop_names(if_node->cond(), names, next_id);
        collect_while_loop_names(if_node->true_branch(), names, next_id);
        collect_while_loop_names(if_node->false_branch(), names, next_id);
        return;
    }

    if (auto* loop = dynamic_cast<LoopNode*>(node)) {
        if (auto* while_loop = dynamic_cast<WhileLoopNode*>(loop)) {
            if (Variable* v = extract_condition_variable(while_loop->condition())) {
                VarKey key{v->name(), v->ssa_version()};
                if (!names.contains(key)) {
                    if (next_id == 0) {
                        names.emplace(std::move(key), "counter");
                    } else {
                        names.emplace(std::move(key), "counter" + std::to_string(next_id));
                    }
                    next_id++;
                }
            }
        }

        collect_while_loop_names(loop->body(), names, next_id);
        return;
    }

    if (auto* sw = dynamic_cast<SwitchNode*>(node)) {
        collect_while_loop_names(sw->cond(), names, next_id);
        for (CaseNode* c : sw->cases()) {
            collect_while_loop_names(c, names, next_id);
        }
        return;
    }

    if (auto* c = dynamic_cast<CaseNode*>(node)) {
        collect_while_loop_names(c->body(), names, next_id);
    }
}

void apply_names(AstNode* node, const std::unordered_map<VarKey, std::string, VarKeyHash>& names) {
    if (!node || names.empty()) return;

    if (auto* code = dynamic_cast<CodeNode*>(node)) {
        if (!code->block()) return;
        for (Instruction* inst : code->block()->instructions()) {
            rename_instruction(inst, names);
        }
        return;
    }

    if (auto* expr_node = dynamic_cast<ExprAstNode*>(node)) {
        rename_expression(expr_node->expr(), names);
        return;
    }

    if (auto* seq = dynamic_cast<SeqNode*>(node)) {
        for (AstNode* child : seq->nodes()) {
            apply_names(child, names);
        }
        return;
    }

    if (auto* if_node = dynamic_cast<IfNode*>(node)) {
        apply_names(if_node->cond(), names);
        apply_names(if_node->true_branch(), names);
        apply_names(if_node->false_branch(), names);
        return;
    }

    if (auto* loop = dynamic_cast<LoopNode*>(node)) {
        rename_expression(loop->condition(), names);
        if (auto* for_loop = dynamic_cast<ForLoopNode*>(loop)) {
            rename_instruction(for_loop->declaration(), names);
            rename_instruction(for_loop->modification(), names);
        }
        apply_names(loop->body(), names);
        return;
    }

    if (auto* sw = dynamic_cast<SwitchNode*>(node)) {
        apply_names(sw->cond(), names);
        for (CaseNode* c : sw->cases()) {
            apply_names(c, names);
        }
        return;
    }

    if (auto* c = dynamic_cast<CaseNode*>(node)) {
        apply_names(c->body(), names);
    }
}

} // namespace

void LoopNameGenerator::apply_for_loop_counters(AbstractSyntaxForest* forest) {
    if (!forest || !forest->root()) {
        return;
    }

    std::unordered_map<VarKey, std::string, VarKeyHash> loop_names;
    std::size_t next_id = 0;
    collect_for_loop_names(forest->root(), loop_names, next_id);
    apply_names(forest->root(), loop_names);
}

void LoopNameGenerator::apply_while_loop_counters(AbstractSyntaxForest* forest) {
    if (!forest || !forest->root()) {
        return;
    }

    std::unordered_map<VarKey, std::string, VarKeyHash> loop_names;
    std::size_t next_id = 0;
    collect_while_loop_names(forest->root(), loop_names, next_id);
    apply_names(forest->root(), loop_names);
}

} // namespace aletheia
