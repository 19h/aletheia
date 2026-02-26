#include "optimization_stages.hpp"
#include <unordered_map>
#include <string>
#include <vector>

namespace dewolf {

static bool contains_variable(Expression* expr, const std::string& var_name) {
    if (!expr) return false;
    
    if (auto* v = dynamic_cast<Variable*>(expr)) {
        return v->name() == var_name;
    }
    
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        for (auto* child : op->operands()) {
            if (contains_variable(child, var_name)) {
                return true;
            }
        }
    }

    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (auto* child : list->operands()) {
            if (contains_variable(child, var_name)) {
                return true;
            }
        }
    }

    return false;
}

static Expression* replace_variable(DecompilerArena& arena, Expression* expr, const std::string& var_name, Expression* replacement) {
    if (!expr) return nullptr;

    if (auto* v = dynamic_cast<Variable*>(expr)) {
        if (v->name() == var_name) {
            return replacement;
        }
        return v;
    }

    if (auto* c = dynamic_cast<Constant*>(expr)) {
        return c;
    }

    if (auto* op = dynamic_cast<Operation*>(expr)) {
        std::vector<Expression*> new_operands;
        bool changed = false;
        for (auto* child : op->operands()) {
            Expression* new_child = replace_variable(arena, child, var_name, replacement);
            if (new_child != child) changed = true;
            new_operands.push_back(new_child);
        }

        if (changed) {
            return arena.create<Operation>(op->type(), std::move(new_operands), op->size_bytes);
        }
        return op;
    }

    return expr;
}

void ExpressionPropagationStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    for (BasicBlock* block : task.cfg()->blocks()) {
        std::vector<Instruction*> new_instructions;
        std::unordered_map<std::string, Expression*> local_defs;

        for (Instruction* inst : block->instructions()) {
            // --- Handle Assignment instructions ---
            if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                Expression* target = assign->destination();
                Expression* source = assign->value();

                // Propagate known definitions into the source (RHS)
                for (const auto& [vname, expr] : local_defs) {
                    source = replace_variable(task.arena(), source, vname, expr);
                }

                if (auto* v = dynamic_cast<Variable*>(target)) {
                    local_defs[v->name()] = source;
                    auto* new_assign = task.arena().create<Assignment>(target, source);
                    new_assign->set_address(assign->address());
                    new_instructions.push_back(new_assign);
                    continue;
                } else {
                    // Memory store: target is a complex expression
                    auto* new_assign = task.arena().create<Assignment>(target, source);
                    new_assign->set_address(assign->address());
                    new_instructions.push_back(new_assign);
                    if (dynamic_cast<Operation*>(target)) {
                        local_defs.clear(); // Memory write invalidates defs
                    }
                }
                continue;
            }

            // --- Handle Branch instructions ---
            if (auto* branch = dynamic_cast<Branch*>(inst)) {
                Condition* cond = branch->condition();
                
                // Propagate into branch condition operands
                Expression* lhs = cond->lhs();
                Expression* rhs = cond->rhs();
                for (const auto& [vname, expr] : local_defs) {
                    lhs = replace_variable(task.arena(), lhs, vname, expr);
                    rhs = replace_variable(task.arena(), rhs, vname, expr);
                }

                // CMP+branch folding: if the LHS was propagated from a subtraction
                // (flags = a - b), fold the CMP operands into the branch condition.
                if (auto* sub_op = dynamic_cast<Operation*>(lhs)) {
                    if (sub_op->type() == OperationType::sub && sub_op->operands().size() == 2) {
                        // Replace: if (flags CMP 0) with if (a CMP b)
                        if (auto* zero = dynamic_cast<Constant*>(rhs)) {
                            if (zero->value() == 0) {
                                lhs = sub_op->operands()[0];
                                rhs = sub_op->operands()[1];
                            }
                        }
                    }
                }

                auto* new_cond = task.arena().create<Condition>(cond->type(), lhs, rhs, cond->size_bytes);
                auto* new_branch = task.arena().create<Branch>(new_cond);
                new_branch->set_address(branch->address());
                new_instructions.push_back(new_branch);
                continue;
            }

            // --- Handle Return instructions ---
            if (auto* ret = dynamic_cast<Return*>(inst)) {
                std::vector<Expression*> new_vals;
                for (auto* val : ret->values()) {
                    Expression* new_val = val;
                    for (const auto& [vname, expr] : local_defs) {
                        new_val = replace_variable(task.arena(), new_val, vname, expr);
                    }
                    new_vals.push_back(new_val);
                }
                auto* new_ret = task.arena().create<Return>(std::move(new_vals));
                new_ret->set_address(ret->address());
                new_instructions.push_back(new_ret);
                continue;
            }

            // --- Everything else (Break, Continue, Comment, Phi) passes through ---
            new_instructions.push_back(inst);
        }
        
        block->set_instructions(std::move(new_instructions));
    }
}

void TypePropagationStage::execute(DecompilerTask& task) {}
void BitFieldComparisonUnrollingStage::execute(DecompilerTask& task) {}
void DeadPathEliminationStage::execute(DecompilerTask& task) {}
void CommonSubexpressionEliminationStage::execute(DecompilerTask& task) {}

} // namespace dewolf
