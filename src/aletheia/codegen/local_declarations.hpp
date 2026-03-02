#pragma once

#include "codegen.hpp"
#include <unordered_set>
#include <map>
#include <set>
#include <string>

namespace aletheia {

class VariableCollector : public DataflowObjectVisitorInterface {
public:
    void visit(Constant* c) override {}
    void visit(Variable* v) override {
        variables_.insert(v);
    }
    void visit(Operation* o) override {
        for (auto* op : o->operands()) if (op) op->accept(*this);
    }
    void visit(Call* c) override {
        if (c->target()) c->target()->accept(*this);
        for (size_t i = 0; i < c->arg_count(); ++i) {
            if (c->arg(i)) c->arg(i)->accept(*this);
        }
    }
    void visit(ListOperation* lo) override {
        for (auto* op : lo->operands()) if (op) op->accept(*this);
    }
    void visit(Condition* c) override {
        if (c->lhs()) c->lhs()->accept(*this);
        if (c->rhs()) c->rhs()->accept(*this);
    }

    void visit_assignment(Assignment* i) override {
        if (i->destination()) i->destination()->accept(*this);
        if (i->value()) i->value()->accept(*this);
    }
    void visit_branch(Branch* i) override {
        if (i->condition()) i->condition()->accept(*this);
    }
    void visit_indirect_branch(IndirectBranch* i) override {
        if (i->expression()) i->expression()->accept(*this);
    }
    void visit_return(Return* i) override {
        for (auto* v : i->values()) if (v) v->accept(*this);
    }
    void visit_phi(Phi* i) override {
        if (i->dest_var()) i->dest_var()->accept(*this);
        if (i->operand_list()) i->operand_list()->accept(*this);
    }
    void visit_relation(Relation* i) override {
        if (i->destination()) i->destination()->accept(*this);
        if (i->value()) i->value()->accept(*this);
    }

    void visit_break(BreakInstr* i) override {}
    void visit_continue(ContinueInstr* i) override {}
    void visit_comment(Comment* i) override {}

    void traverse(AstNode* node) {
        if (!node) return;
        if (auto* expr_node = ast_dyn_cast<ExprAstNode>(node)) {
            if (expr_node->expr()) expr_node->expr()->accept(*this);
        } else if (auto* cnode = ast_dyn_cast<CodeNode>(node)) {
            if (cnode->block()) {
                for (auto* inst : cnode->block()->instructions()) {
                    if (inst) inst->accept(*this);
                }
            }
        } else if (auto* snode = ast_dyn_cast<SeqNode>(node)) {
            for (auto* child : snode->nodes()) traverse(child);
        } else if (auto* inode = ast_dyn_cast<IfNode>(node)) {
            traverse(inode->cond());
            traverse(inode->true_branch());
            traverse(inode->false_branch());
        } else if (auto* lnode = ast_dyn_cast<LoopNode>(node)) {
            if (lnode->condition()) lnode->condition()->accept(*this);
            
            // ForLoopNode might have declaration and modification instructions
            if (auto* fnode = ast_dyn_cast<ForLoopNode>(node)) {
                if (fnode->declaration()) fnode->declaration()->accept(*this);
                if (fnode->modification()) fnode->modification()->accept(*this);
            }
            
            traverse(lnode->body());
        } else if (auto* swnode = ast_dyn_cast<SwitchNode>(node)) {
            traverse(swnode->cond());
            for (auto* c : swnode->cases()) traverse(c);
        } else if (auto* casenode = ast_dyn_cast<CaseNode>(node)) {
            traverse(casenode->body());
        }
    }

    const std::unordered_set<Variable*>& variables() const { return variables_; }

private:
    std::unordered_set<Variable*> variables_;
};

class LocalDeclarationGenerator {
public:
    static std::vector<std::string> generate(DecompilerTask& task, CExpressionGenerator& expr_gen) {
        VariableCollector collector;
        if (task.ast() && task.ast()->root()) {
            collector.traverse(task.ast()->root());
        }

        // Build the set of parameter names to exclude from local declarations.
        auto param_names = task.parameter_names();

        // Group by type string -> sorted set of variable names.
        std::map<std::string, std::set<std::string>> type_to_vars;
        
        for (auto* var : collector.variables()) {
            // Skip global variables.
            if (isa<GlobalVariable>(var)) {
                continue;
            }
            
            // Skip parameter variables (they appear in the function signature).
            if (var->is_parameter()) {
                continue;
            }

            std::string var_name = expr_gen.generate(var);

            // Also skip by name match against parameter names (catches cases
            // where the Variable node wasn't tagged but shares a parameter name).
            if (param_names.contains(var_name)) {
                continue;
            }
            
            std::string type_str = "int"; // Default fallback.
            if (var->ir_type()) {
                type_str = var->ir_type()->to_string();
            }
            type_to_vars[type_str].insert(var_name);
        }

        std::vector<std::string> decls;
        for (const auto& [type_str, vars] : type_to_vars) {
            std::string line = type_str + " ";
            bool first = true;
            for (const auto& v : vars) {
                if (!first) line += ", ";
                line += v;
                first = false;
            }
            line += ";";
            decls.push_back(line);
        }
        
        return decls;
    }
};

class GlobalDeclarationGenerator {
public:
    static std::vector<std::string> generate(DecompilerTask& task) {
        VariableCollector collector;
        if (task.ast() && task.ast()->root()) {
            collector.traverse(task.ast()->root());
        }

        std::map<std::string, GlobalVariable*> globals_by_name;
        for (auto* var : collector.variables()) {
            auto* gv = dyn_cast<GlobalVariable>(var);
            if (!gv) continue;
            globals_by_name.try_emplace(gv->name(), gv);
        }

        std::vector<std::string> decls;
        for (const auto& [name, gv] : globals_by_name) {
            std::string type_str = "int";
            if (gv->ir_type()) {
                type_str = gv->ir_type()->to_string();
            }

            std::string line = "extern ";
            if (gv->is_constant()) {
                line += "const ";
            }
            line += type_str + " " + name + ";";
            decls.push_back(std::move(line));
        }

        return decls;
    }
};

} // namespace aletheia
