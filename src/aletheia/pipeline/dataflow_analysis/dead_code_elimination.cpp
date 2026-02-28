#include "dead_code_elimination.hpp"
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

namespace aletheia {

static void extract_uses(Expression* expr, std::unordered_set<std::string>& uses) {
    if (!expr) return;
    if (Variable* v = dynamic_cast<Variable*>(expr)) {
        uses.insert(v->name() + "_" + std::to_string(v->ssa_version()));
    } else if (Operation* op = dynamic_cast<Operation*>(expr)) {
        for (Expression* child : op->operands()) {
            extract_uses(child, uses);
        }
    } else if (ListOperation* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression* child : list->operands()) {
            extract_uses(child, uses);
        }
    }
}

void DeadCodeEliminationStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    bool changed = true;
    while (changed) {
        changed = false;
        
        std::unordered_set<std::string> global_uses;

        // Pass 1: Collect all variable uses across the CFG
        for (BasicBlock* bb : task.cfg()->blocks()) {
            for (Instruction* inst : bb->instructions()) {
                if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                    // Uses from the RHS
                    extract_uses(assign->value(), global_uses);
                    // If destination is a complex expression (e.g., deref), its
                    // sub-expressions are also uses
                    if (!dynamic_cast<Variable*>(assign->destination())) {
                        extract_uses(assign->destination(), global_uses);
                    }
                } else if (auto* branch = dynamic_cast<Branch*>(inst)) {
                    extract_uses(branch->condition(), global_uses);
                } else if (auto* ret = dynamic_cast<Return*>(inst)) {
                    for (auto* val : ret->values()) {
                        extract_uses(val, global_uses);
                    }
                } else if (auto* phi = dynamic_cast<Phi*>(inst)) {
                    // Phi operands are uses
                    if (phi->operand_list()) {
                        extract_uses(phi->operand_list(), global_uses);
                    }
                }
                // Break, Continue, Comment have no variable references
            }
        }

        // Pass 2: Remove assignments whose definitions are not used
        for (BasicBlock* bb : task.cfg()->blocks()) {
            std::vector<Instruction*> new_insts;
            for (Instruction* inst : bb->instructions()) {
                if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                    // Don't eliminate phis in this pass
                    if (dynamic_cast<Phi*>(inst)) {
                        new_insts.push_back(inst);
                        continue;
                    }
                    
                    if (Variable* target = dynamic_cast<Variable*>(assign->destination())) {
                        std::string def_name = target->name() + "_" + std::to_string(target->ssa_version());
                        
                        // Don't eliminate definitions of special registers (return values, stack pointer)
                        if (!global_uses.contains(def_name) && 
                            target->name() != "SP" && target->name() != "W0" && target->name() != "X0") {
                            changed = true;
                            continue; // Drop instruction
                        }
                    }
                }
                new_insts.push_back(inst);
            }
            if (new_insts.size() != bb->instructions().size()) {
                bb->set_instructions(std::move(new_insts));
            }
        }
    }
}

} // namespace aletheia
