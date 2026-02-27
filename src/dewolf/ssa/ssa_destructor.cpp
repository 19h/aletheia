#include "ssa_destructor.hpp"
#include "phi_dependency_resolver.hpp"
#include "minimal_variable_renamer.hpp"
#include "liveness/liveness.hpp"
#include "../pipeline/pipeline.hpp"
#include <vector>

namespace dewolf {

void SsaDestructor::execute(DecompilerTask& task) {
    if (!task.cfg()) return;
    PhiDependencyResolver::resolve(task.arena(), *task.cfg());
    LivenessAnalysis liveness(*task.cfg());
    eliminate_phi_nodes(task.arena(), *task.cfg(), liveness);
    MinimalVariableRenamer::rename(task.arena(), *task.cfg());
}

void SsaDestructor::eliminate_phi_nodes(DecompilerArena& arena, ControlFlowGraph& cfg, const LivenessAnalysis& liveness) {
    for (BasicBlock* bb : cfg.blocks()) {
        std::vector<Instruction*> new_insts;
        
        for (Instruction* inst : bb->instructions()) {
            auto* phi = dynamic_cast<Phi*>(inst);
            if (!phi) {
                new_insts.push_back(inst);
                continue;
            }

            // Process phi node: insert copy operations in predecessor blocks
            Variable* target = phi->dest_var();
            if (!target) continue;

            auto* op_list = phi->operand_list();
            if (!op_list || op_list->empty()) continue;

            // Use origin_block if available, otherwise fall back to positional matching
            if (!phi->origin_block().empty()) {
                for (auto& [pred_block, source_expr] : phi->origin_block()) {
                    if (!source_expr) continue;

                    // Check interference: is the target live-out at the predecessor?
                    bool interference = liveness.live_out(pred_block).contains(target->name());

                    Expression* final_target = target;
                    if (interference) {
                        // Create a temporary to break the interference
                        auto* tmp = arena.create<Variable>(target->name() + "_tmp", target->size_bytes);
                        final_target = tmp;
                        
                        // Insert copy from tmp to real target at the start of the phi's block
                        auto* bb_copy = arena.create<Assignment>(target, tmp);
                        new_insts.push_back(bb_copy);
                    }

                    // Insert copy at the end of predecessor block (before branch if present)
                    auto* copy_assign = arena.create<Assignment>(final_target, source_expr);
                    auto pred_insts = pred_block->instructions();
                    
                    if (!pred_insts.empty()) {
                        Instruction* last_inst = pred_insts.back();
                        // Insert before branch/return at end of predecessor
                        if (is_branch(last_inst) || is_return(last_inst)) {
                            pred_insts.insert(pred_insts.end() - 1, copy_assign);
                        } else {
                            pred_insts.push_back(copy_assign);
                        }
                    } else {
                        pred_insts.push_back(copy_assign);
                    }
                    
                    pred_block->set_instructions(std::move(pred_insts));
                }
            } else {
                // Fallback: positional matching (predecessor index -> operand index)
                for (size_t i = 0; i < op_list->operands().size() && i < bb->predecessors().size(); ++i) {
                    Expression* source = op_list->operands()[i];
                    BasicBlock* pred = bb->predecessors()[i]->source();
                    
                    bool interference = liveness.live_out(pred).contains(target->name());

                    Expression* final_target = target;
                    if (interference) {
                        auto* tmp = arena.create<Variable>(target->name() + "_tmp", target->size_bytes);
                        final_target = tmp;
                        auto* bb_copy = arena.create<Assignment>(target, tmp);
                        new_insts.push_back(bb_copy);
                    }

                    auto* copy_assign = arena.create<Assignment>(final_target, source);
                    auto pred_insts = pred->instructions();
                    
                    if (!pred_insts.empty()) {
                        Instruction* last_inst = pred_insts.back();
                        if (is_branch(last_inst) || is_return(last_inst)) {
                            pred_insts.insert(pred_insts.end() - 1, copy_assign);
                        } else {
                            pred_insts.push_back(copy_assign);
                        }
                    } else {
                        pred_insts.push_back(copy_assign);
                    }
                    
                    pred->set_instructions(std::move(pred_insts));
                }
            }
        }
        
        bb->set_instructions(std::move(new_insts));
    }
}

} // namespace dewolf
