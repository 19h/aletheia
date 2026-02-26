#include "ssa_destructor.hpp"
#include "liveness/liveness.hpp"
#include "../pipeline/pipeline.hpp"
#include <vector>

namespace dewolf {

void SsaDestructor::execute(DecompilerTask& task) {
    if (!task.cfg()) return;
    LivenessAnalysis liveness(*task.cfg());
    eliminate_phi_nodes(task.arena(), *task.cfg(), liveness);
}

void SsaDestructor::eliminate_phi_nodes(DecompilerArena& arena, ControlFlowGraph& cfg, const LivenessAnalysis& liveness) {
    // True Sreedhar Out-of-SSA Translation
    
    for (BasicBlock* bb : cfg.blocks()) {
        std::vector<Instruction*> new_insts;
        
        for (Instruction* inst : bb->instructions()) {
            Operation* op = inst->operation();
            if (op->type() == OperationType::phi) {
                if (op->operands().size() > 1) {
                    Expression* target = op->operands()[0];

                    for (size_t i = 1; i < op->operands().size(); ++i) {
                        Expression* source = op->operands()[i];
                        
                        // We need the corresponding predecessor block.
                        if (i - 1 < bb->predecessors().size()) {
                            BasicBlock* pred = bb->predecessors()[i - 1]->source();
                            
                            // Check interference:
                            // Sreedhar's method handles Live-Out bounds.
                            bool interference = false;
                            if (auto* t_var = dynamic_cast<Variable*>(target)) {
                                if (liveness.live_out(pred).contains(t_var->name())) {
                                    interference = true;
                                }
                            }

                            // Parallel copy emulation
                            Expression* final_target = target;
                            if (interference) {
                                // If the target is live at the end of the predecessor, writing directly to it
                                // would clobber its value before the original use is finished!
                                // We must emit a temporary parallel copy resolving the interference.
                                if (auto* t_var = dynamic_cast<Variable*>(target)) {
                                    final_target = arena.create<Variable>(t_var->name() + "_tmp", t_var->size_bytes);
                                    
                                    // And we must also inject a copy from the tmp to the real target 
                                    // at the start of our current block (bb)!
                                    Operation* bb_copy_op = arena.create<Operation>(OperationType::assign,
                                        std::vector<Expression*>{target, final_target}, target->size_bytes);
                                    new_insts.push_back(arena.create<Instruction>(0, bb_copy_op));
                                }
                            }

                            // Insert copy at the END of predecessor block
                            Operation* copy_op = arena.create<Operation>(OperationType::assign,
                                std::vector<Expression*>{final_target, source}, target->size_bytes);
                            Instruction* copy_inst = arena.create<Instruction>(0, copy_op);
                            
                            auto pred_insts = pred->instructions();
                            
                            // If the last instruction is a branch (cmp, b.le, ret, call), insert BEFORE it
                            if (!pred_insts.empty()) {
                                Instruction* last_inst = pred_insts.back();
                                OperationType last_type = last_inst->operation()->type();
                                if (last_type >= OperationType::eq || last_type == OperationType::call) {
                                    pred_insts.insert(pred_insts.end() - 1, copy_inst);
                                } else {
                                    pred_insts.push_back(copy_inst);
                                }
                            } else {
                                pred_insts.push_back(copy_inst);
                            }
                            
                            pred->set_instructions(std::move(pred_insts));
                        }
                    }
                }
            } else {
                new_insts.push_back(inst);
            }
        }
        
        bb->set_instructions(std::move(new_insts));
    }
}

} // namespace dewolf
