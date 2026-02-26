#include "optimization_stages.hpp"
#include <unordered_map>
#include <string>
#include <vector>

namespace dewolf {

// Helper to check if an expression contains a specific variable
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

// Helper to replace a variable with another expression
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
    // Basic local block expression propagation
    if (!task.cfg()) return;

    for (BasicBlock* block : task.cfg()->blocks()) {
        std::vector<Instruction*> new_instructions;
        std::unordered_map<std::string, Expression*> local_defs;

        for (Instruction* inst : block->instructions()) {
            Operation* op = inst->operation();
            
            // Apply current substitutions to RHS of the operation
            if (op->type() == OperationType::assign && op->operands().size() == 2) {
                Expression* target = op->operands()[0];
                Expression* source = op->operands()[1];

                // Substitute any known definitions into the source
                for (const auto& [vname, expr] : local_defs) {
                    source = replace_variable(task.arena(), source, vname, expr);
                }

                if (auto* v = dynamic_cast<Variable*>(target)) {
                    // It's an assignment to a variable: record it
                    // E.g., W8 = *(mem_0) - 5
                    local_defs[v->name()] = source;
                    // We don't push it yet, we might completely inline it
                    continue; 
                } else {
                    // Memory store or something else, write it with updated source
                    Operation* new_op = task.arena().create<Operation>(OperationType::assign, 
                        std::vector<Expression*>{target, source}, op->size_bytes);
                    new_instructions.push_back(task.arena().create<Instruction>(inst->address(), new_op));
                    // Invalidate definitions that might be aliased (simplification: clear all for memory)
                    if (dynamic_cast<Operation*>(target)) {
                        local_defs.clear();
                    }
                }
            } else if (op->type() == OperationType::call) {
                // Substitute arguments
                std::vector<Expression*> new_ops;
                new_ops.push_back(op->operands()[0]); // Function target
                for (size_t i = 1; i < op->operands().size(); ++i) {
                    Expression* arg = op->operands()[i];
                    for (const auto& [vname, expr] : local_defs) {
                        arg = replace_variable(task.arena(), arg, vname, expr);
                    }
                    new_ops.push_back(arg);
                }
                
                Operation* new_op = task.arena().create<Operation>(op->type(), std::move(new_ops), op->size_bytes);
                new_instructions.push_back(task.arena().create<Instruction>(inst->address(), new_op));

                // Calls clobber registers/memory
                local_defs.clear();
            } else {
                // Branch or other
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
        
        // If there are left over definitions that are live-out, we'd need to emit them here.
        // For simplicity, we assume we just emit them. In full DeWolf, SSA naturally handles live-out.
        for (const auto& [vname, expr] : local_defs) {
            Variable* v = task.arena().create<Variable>(vname, expr->size_bytes);
            Operation* new_op = task.arena().create<Operation>(OperationType::assign, 
                std::vector<Expression*>{v, expr}, expr->size_bytes);
            new_instructions.push_back(task.arena().create<Instruction>(0, new_op));
        }

        // Replace block instructions
        block->set_instructions(std::move(new_instructions));
    }
}

void TypePropagationStage::execute(DecompilerTask& task) {
    // Propagate types through dataflow
}

void BitFieldComparisonUnrollingStage::execute(DecompilerTask& task) {
    // Unroll bitfield extractions
}

void DeadPathEliminationStage::execute(DecompilerTask& task) {
    // Invoke logic engine to find unreachable branches and remove them
}

void DeadCodeEliminationStage::execute(DecompilerTask& task) {
    // Remove unused definitions
}

void DeadLoopEliminationStage::execute(DecompilerTask& task) {
    // Detect and remove infinite empty loops or loops with no side effects
}

void CommonSubexpressionEliminationStage::execute(DecompilerTask& task) {
    // Identify common subexpressions and assign them to temporaries
}

} // namespace dewolf
