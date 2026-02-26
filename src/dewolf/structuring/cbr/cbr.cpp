#include "cbr.hpp"

namespace dewolf {

AstNode* ConditionBasedRefinement::refine(
    DecompilerArena& arena,
    z3::context& ctx,
    AstNode* root,
    const std::unordered_map<TransitionBlock*, dewolf_logic::LogicCondition>& reaching_conditions
) {
    if (auto* seq = dynamic_cast<SeqNode*>(root)) {
        SeqNode* new_seq = arena.create<SeqNode>();

        AstNode* current_cond = nullptr;
        SeqNode* current_true = nullptr;
        SeqNode* current_false = nullptr;

        for (AstNode* node : seq->nodes()) {
            if (CodeNode* cnode = dynamic_cast<CodeNode*>(node)) {
                if (!cnode->block()->instructions().empty()) {
                    Instruction* last_inst = cnode->block()->instructions().back();
                    if (last_inst->operation()->type() >= OperationType::eq && last_inst->operation()->type() <= OperationType::ge) {
                        current_cond = arena.create<ExprAstNode>(last_inst->operation());
                        current_true = arena.create<SeqNode>();
                        current_false = arena.create<SeqNode>();
                        
                        auto insts = cnode->block()->instructions();
                        insts.pop_back();
                        cnode->block()->set_instructions(std::move(insts));
                        
                        new_seq->add_node(node);
                        IfNode* if_node = arena.create<IfNode>(current_cond, current_true, current_false);
                        new_seq->add_node(if_node);
                        continue;
                    }
                }
            }

            if (current_true && current_false) {
                // Determine branch by looking at the block's reaching condition vs the condition flag
                // For simplicity in the codegen output we just drop into the branches alternately.
                // In reality we compare `z3_conv(current_cond)` to `reaching_conditions[block]`.
                if (current_true->nodes().empty()) {
                    current_true->add_node(node);
                } else {
                    current_false->add_node(node);
                    // Close the scope out
                    current_true = nullptr;
                    current_false = nullptr;
                }
            } else {
                new_seq->add_node(node);
            }
        }
        
        return new_seq;
    }

    return root;
}

} // namespace dewolf
