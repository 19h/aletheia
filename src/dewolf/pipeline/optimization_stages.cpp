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
            Operation* op = inst->operation();
            
            if (op->type() == OperationType::assign && op->operands().size() == 2) {
                Expression* target = op->operands()[0];
                Expression* source = op->operands()[1];

                for (const auto& [vname, expr] : local_defs) {
                    source = replace_variable(task.arena(), source, vname, expr);
                }

                if (auto* v = dynamic_cast<Variable*>(target)) {
                    // Update definition locally but DO NOT inline away side-effectful or multi-use variables completely
                    // DeWolf's GraphExpressionFolding does this strictly over SSA chains
                    local_defs[v->name()] = source;
                    // We must still emit the SSA assignment for visibility!
                    Operation* new_op = task.arena().create<Operation>(OperationType::assign, 
                        std::vector<Expression*>{target, source}, op->size_bytes);
                    new_instructions.push_back(task.arena().create<Instruction>(inst->address(), new_op));
                    continue; 
                } else {
                    Operation* new_op = task.arena().create<Operation>(OperationType::assign, 
                        std::vector<Expression*>{target, source}, op->size_bytes);
                    new_instructions.push_back(task.arena().create<Instruction>(inst->address(), new_op));
                    if (dynamic_cast<Operation*>(target)) {
                        local_defs.clear();
                    }
                }
            } else if (op->type() == OperationType::call) {
                std::vector<Expression*> new_ops;
                new_ops.push_back(op->operands()[0]);
                for (size_t i = 1; i < op->operands().size(); ++i) {
                    Expression* arg = op->operands()[i];
                    for (const auto& [vname, expr] : local_defs) {
                        arg = replace_variable(task.arena(), arg, vname, expr);
                    }
                    new_ops.push_back(arg);
                }
                
                Operation* new_op = task.arena().create<Operation>(op->type(), std::move(new_ops), op->size_bytes);
                new_instructions.push_back(task.arena().create<Instruction>(inst->address(), new_op));
                local_defs.clear();
            } else if (op->type() >= OperationType::eq && op->type() <= OperationType::ge) {
                // Combine flags/conditions (CMP + B.LE)
                // A branch operation itself!
                std::vector<Expression*> new_ops;
                for (auto* arg : op->operands()) {
                    for (const auto& [vname, expr] : local_defs) {
                        arg = replace_variable(task.arena(), arg, vname, expr);
                    }
                    new_ops.push_back(arg);
                }

                // If previous instruction was a CMP (sub), fold its operands into this branch!
                if (!new_instructions.empty()) {
                    Instruction* last_inst = new_instructions.back();
                    if (last_inst->operation()->type() == OperationType::sub && last_inst->operation()->operands().size() == 2) {
                        // Fold it! (e.g. b.le becomes (a <= b))
                        std::vector<Expression*> cmp_ops = last_inst->operation()->operands();
                        Operation* cond_op = task.arena().create<Operation>(op->type(), std::move(cmp_ops), 0);
                        
                        // Replace the cmp and the branch with just the branch holding the condition
                        new_instructions.pop_back();
                        new_instructions.push_back(task.arena().create<Instruction>(inst->address(), cond_op));
                        continue;
                    } else if (last_inst->operation()->type() == OperationType::assign && last_inst->operation()->operands().size() == 2) {
                        // Sometimes CMP is folded into an assign if mapped incorrectly, let's look for a sub inside the assign
                        if (auto* rhs_sub = dynamic_cast<Operation*>(last_inst->operation()->operands()[1])) {
                            if (rhs_sub->type() == OperationType::sub && rhs_sub->operands().size() == 2) {
                                std::vector<Expression*> cmp_ops = rhs_sub->operands();
                                Operation* cond_op = task.arena().create<Operation>(op->type(), std::move(cmp_ops), 0);
                                new_instructions.pop_back();
                                new_instructions.push_back(task.arena().create<Instruction>(inst->address(), cond_op));
                                continue;
                            }
                        }
                    }
                }
                
                Operation* new_op = task.arena().create<Operation>(op->type(), std::move(new_ops), op->size_bytes);
                new_instructions.push_back(task.arena().create<Instruction>(inst->address(), new_op));
            } else {
                std::vector<Expression*> new_ops;
                for (auto* arg : op->operands()) {
                    for (const auto& [vname, expr] : local_defs) {
                        arg = replace_variable(task.arena(), arg, vname, expr);
                    }
                    new_ops.push_back(arg);
                }
                Operation* new_op = task.arena().create<Operation>(op->type(), std::move(new_ops), op->size_bytes);
                new_instructions.push_back(task.arena().create<Instruction>(inst->address(), new_op));
            }
        }
        
        block->set_instructions(std::move(new_instructions));
    }
}

void TypePropagationStage::execute(DecompilerTask& task) {}
void BitFieldComparisonUnrollingStage::execute(DecompilerTask& task) {}
void DeadPathEliminationStage::execute(DecompilerTask& task) {}
void CommonSubexpressionEliminationStage::execute(DecompilerTask& task) {}

} // namespace dewolf
