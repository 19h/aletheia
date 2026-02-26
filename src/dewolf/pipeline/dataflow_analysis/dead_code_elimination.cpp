#include "dead_code_elimination.hpp"
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

namespace dewolf {

static void extract_uses(Expression* expr, std::unordered_set<std::string>& uses) {
    if (!expr) return;
    if (Variable* v = dynamic_cast<Variable*>(expr)) {
        uses.insert(v->name() + "_" + std::to_string(v->ssa_version()));
    } else if (Operation* op = dynamic_cast<Operation*>(expr)) {
        for (Expression* child : op->operands()) {
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

        for (BasicBlock* bb : task.cfg()->blocks()) {
            for (Instruction* inst : bb->instructions()) {
                Operation* op = inst->operation();
                if (!op) continue;
                
                if (op->type() == OperationType::assign && op->operands().size() == 2) {
                    extract_uses(op->operands()[1], global_uses);
                    if (!dynamic_cast<Variable*>(op->operands()[0])) {
                        extract_uses(op->operands()[0], global_uses);
                    }
                } else {
                    extract_uses(op, global_uses);
                }
            }
        }

        for (BasicBlock* bb : task.cfg()->blocks()) {
            std::vector<Instruction*> new_insts;
            for (Instruction* inst : bb->instructions()) {
                Operation* op = inst->operation();
                if (!op) continue;

                if (op->type() == OperationType::assign && op->operands().size() == 2) {
                    if (Variable* target = dynamic_cast<Variable*>(op->operands()[0])) {
                        std::string def_name = target->name() + "_" + std::to_string(target->ssa_version());
                        
                        if (!global_uses.contains(def_name) && target->name() != "SP" && target->name() != "W0" && target->name() != "X0") {
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

} // namespace dewolf
