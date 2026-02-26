#include "cbr.hpp"
#include <vector>
#include <unordered_set>

namespace dewolf {

AstNode* ConditionBasedRefinement::refine(
    DecompilerArena& arena,
    z3::context& ctx,
    AstNode* root,
    const std::unordered_map<TransitionBlock*, dewolf_logic::LogicCondition>& reaching_conditions
) {
    if (auto* seq = dynamic_cast<SeqNode*>(root)) {
        // We will build a hierarchical AST from the flat sequence based on boolean equivalences.
        // This is a naive implementation of DeWolf's cluster_by_condition recursively building branches.
        
        std::vector<AstNode*> nodes = seq->nodes();
        if (nodes.empty()) return root;

        // Group 1: The nodes executed unconditionally (reaching condition == True)
        // Group 2: The nodes executed conditionally
        SeqNode* new_seq = arena.create<SeqNode>();

        for (size_t i = 0; i < nodes.size(); ) {
            AstNode* node = nodes[i];
            
            // Check if this node is a Condition extraction point
            AstNode* branch_cond = nullptr;
            if (CodeNode* cnode = dynamic_cast<CodeNode*>(node)) {
                if (!cnode->block()->instructions().empty()) {
                    Instruction* last_inst = cnode->block()->instructions().back();
                    if (last_inst->operation()->type() >= OperationType::eq && last_inst->operation()->type() <= OperationType::ge) {
                        branch_cond = arena.create<ExprAstNode>(last_inst->operation());
                        auto insts = cnode->block()->instructions();
                        insts.pop_back();
                        cnode->block()->set_instructions(std::move(insts));
                        
                        dewolf_logic::Z3Converter conv(ctx);
                        dewolf_logic::LogicCondition extracted_cond = conv.convert_to_condition(last_inst->operation());
                        
                        new_seq->add_node(node);
                        i++;

                        // Now collect all subsequent nodes that match `extracted_cond` into True
                        // and `!extracted_cond` into False
                        SeqNode* true_branch = arena.create<SeqNode>();
                        SeqNode* false_branch = arena.create<SeqNode>();
                        
                        while (i < nodes.size()) {
                            AstNode* next_node = nodes[i];
                            // Find matching TransitionBlock to check reaching condition
                            TransitionBlock* matching_tb = nullptr;
                            for (const auto& [tb, cond] : reaching_conditions) {
                                if (tb->ast_node() == next_node || tb->ast_node()->get_original_block() == next_node->get_original_block()) {
                                    matching_tb = tb;
                                    break;
                                }
                            }

                            if (matching_tb) {
                                auto& rc = reaching_conditions.at(matching_tb);
                                // The condition we extract evaluates whether the block's reaching condition
                                // implies our conditional jump, or vice versa, to fold the ast.
                                if (rc.does_imply(extracted_cond) || extracted_cond.does_imply(rc)) {
                                    true_branch->add_node(next_node);
                                } else if (rc.is_complementary_to(extracted_cond)) {
                                    false_branch->add_node(next_node);
                                } else {
                                    // Breaks condition grouping, belongs to outer scope
                                    break;
                                }
                            } else {
                                break;
                            }
                            i++;
                        }

                        if (!true_branch->nodes().empty() || !false_branch->nodes().empty()) {
                            IfNode* if_node = arena.create<IfNode>(branch_cond, true_branch, false_branch->nodes().empty() ? nullptr : false_branch);
                            new_seq->add_node(if_node);
                        }
                        
                        continue; // Outer loop advances based on inner while
                    }
                }
            }

            new_seq->add_node(node);
            i++;
        }
        
        return new_seq;
    }

    return root;
}

} // namespace dewolf
