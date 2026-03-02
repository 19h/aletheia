#include "graph_expression_folding.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aletheia {

static void substitute_uses(Expression* expr, const std::unordered_map<Variable*, Expression*>& subs) {
    if (!expr) return;
    if (auto* op = dyn_cast<Operation>(expr)) {
        for (size_t i = 0; i < op->operands().size(); ++i) {
            if (auto* v = dyn_cast<Variable>(op->operands()[i])) {
                auto it = subs.find(v);
                if (it != subs.end()) {
                    op->mutable_operands()[i] = it->second;
                }
            } else {
                substitute_uses(op->operands()[i], subs);
            }
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (size_t i = 0; i < list->operands().size(); ++i) {
            if (auto* v = dyn_cast<Variable>(list->operands()[i])) {
                auto it = subs.find(v);
                if (it != subs.end()) {
                    list->mutable_operands()[i] = it->second;
                }
            } else {
                substitute_uses(list->operands()[i], subs);
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

    // Pass 1: Build identity groups from Assignments (not Phis)
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            // Skip phi nodes
            if (isa<Phi>(inst)) continue;

            if (auto* assign = dyn_cast<Assignment>(inst)) {
                if (auto* target = dyn_cast<Variable>(assign->destination())) {
                    Expression* value = assign->value();
                    
                    // If assigning to a Constant or another Variable without side effects
                    if (isa<Constant>(value) || isa<Variable>(value)) {
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
            if (auto* v_val = dyn_cast<Variable>(value)) {
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

            // Skip phi nodes (don't substitute naively into phis)
            if (isa<Phi>(inst)) {
                new_insts.push_back(inst);
                continue;
            }

            if (auto* assign = dyn_cast<Assignment>(inst)) {
                // Substitute in the value (RHS) only
                Expression* value = assign->value();
                if (auto* v = dyn_cast<Variable>(value)) {
                    auto it = substitutions.find(v);
                    if (it != substitutions.end()) {
                        assign->set_value(it->second);
                    }
                } else {
                    substitute_uses(value, substitutions);
                }
            } else if (auto* branch = dyn_cast<Branch>(inst)) {
                // Substitute in branch condition
                substitute_uses(branch->condition(), substitutions);
            } else if (auto* ret = dyn_cast<Return>(inst)) {
                // Return values: substitute handled via expression walk
                for (auto* val : ret->values()) {
                    substitute_uses(val, substitutions);
                }
            }
            
            new_insts.push_back(inst);
        }
        block->set_instructions(std::move(new_insts));
    }
}

} // namespace aletheia
