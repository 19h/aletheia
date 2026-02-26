#include "graph_expression_folding.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dewolf {

static void substitute_uses(Expression* expr, const std::unordered_map<Variable*, Expression*>& subs) {
    if (!expr) return;
    if (Operation* op = dynamic_cast<Operation*>(expr)) {
        for (size_t i = 0; i < op->operands().size(); ++i) {
            if (Variable* v = dynamic_cast<Variable*>(op->operands()[i])) {
                auto it = subs.find(v);
                if (it != subs.end()) {
                    op->mutable_operands()[i] = it->second;
                }
            } else {
                substitute_uses(op->operands()[i], subs);
            }
        }
    }
}

void GraphExpressionFoldingStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    // True Global Expression Graph Folding
    // Identity mapping Phase
    std::unordered_map<Variable*, Expression*> substitutions;
    std::unordered_set<Instruction*> dead_instructions;

    // Pass 1: Build identity groups from Assignments
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            Operation* op = inst->operation();
            if (op->type() == OperationType::assign && op->operands().size() == 2) {
                if (Variable* target = dynamic_cast<Variable*>(op->operands()[0])) {
                    Expression* value = op->operands()[1];
                    
                    // If assigning to a Constant or another Variable without side effects
                    if (dynamic_cast<Constant*>(value) || dynamic_cast<Variable*>(value)) {
                        substitutions[target] = value;
                        dead_instructions.insert(inst);
                    }
                }
            }
        }
    }

    // Resolve chains (e.g. A = B, B = C -> A = C)
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [target, value] : substitutions) {
            if (Variable* v_val = dynamic_cast<Variable*>(value)) {
                if (substitutions.contains(v_val)) {
                    substitutions[target] = substitutions[v_val];
                    changed = true;
                }
            }
        }
    }

    // Pass 2: Apply substitutions globally and wipe dead code
    for (BasicBlock* block : task.cfg()->blocks()) {
        std::vector<Instruction*> new_insts;
        for (Instruction* inst : block->instructions()) {
            if (dead_instructions.contains(inst)) {
                continue;
            }

            Operation* op = inst->operation();
            
            // For assignments, we only substitute the RHS (operands[1])
            if (op->type() == OperationType::assign && op->operands().size() == 2) {
                if (Variable* v = dynamic_cast<Variable*>(op->operands()[1])) {
                    auto it = substitutions.find(v);
                    if (it != substitutions.end()) {
                        op->mutable_operands()[1] = it->second;
                    }
                } else {
                    substitute_uses(op->operands()[1], substitutions);
                }
            } else if (op->type() != OperationType::phi) {
                // Do not substitute phi targets/arguments naively without proper SSA tracking
                substitute_uses(op, substitutions);
            }
            
            new_insts.push_back(inst);
        }
        block->set_instructions(std::move(new_insts));
    }
}

} // namespace dewolf
