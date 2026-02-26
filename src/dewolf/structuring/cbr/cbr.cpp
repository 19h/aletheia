#include "cbr.hpp"
#include <vector>
#include <unordered_set>
#include <iostream>

namespace dewolf {

AstNode* ConditionBasedRefinement::refine(
    DecompilerArena& arena,
    z3::context& ctx,
    AstNode* root,
    const std::unordered_map<TransitionBlock*, dewolf_logic::LogicCondition>& reaching_conditions
) {
    if (auto* seq = dynamic_cast<SeqNode*>(root)) {
        std::vector<AstNode*> nodes = seq->nodes();
        if (nodes.empty()) return root;

        SeqNode* new_seq = arena.create<SeqNode>();

        for (size_t i = 0; i < nodes.size(); ) {
            AstNode* node = nodes[i];
            
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

                        SeqNode* true_branch = arena.create<SeqNode>();
                        SeqNode* false_branch = arena.create<SeqNode>();
                        
                        while (i < nodes.size()) {
                            AstNode* next_node = nodes[i];
                            TransitionBlock* matching_tb = nullptr;
                            for (const auto& [tb, cond] : reaching_conditions) {
                                if (tb->ast_node() == next_node || tb->ast_node()->get_original_block() == next_node->get_original_block()) {
                                    matching_tb = tb;
                                    break;
                                }
                            }

                            if (matching_tb) {
                                auto& rc = reaching_conditions.at(matching_tb);
                                if (rc.does_imply(extracted_cond) || extracted_cond.does_imply(rc)) {
                                    true_branch->add_node(next_node);
                                } else if (rc.is_complementary_to(extracted_cond)) {
                                    false_branch->add_node(next_node);
                                } else {
                                    std::cerr << "[CBR Debug] Node " << i << " broke out! Condition did not imply/complement." << std::endl;
                                    std::cerr << "rc: " << rc.expression() << " | extracted: " << extracted_cond.expression() << std::endl;
                                    break;
                                }
                            } else {
                                std::cerr << "[CBR Debug] Node " << i << " broke out! No matching_tb found!" << std::endl;
                                break;
                            }
                            i++;
                        }

                        if (!true_branch->nodes().empty() || !false_branch->nodes().empty()) {
                            IfNode* if_node = arena.create<IfNode>(branch_cond, true_branch, false_branch->nodes().empty() ? nullptr : false_branch);
                            new_seq->add_node(if_node);
                        } else {
                            std::cerr << "[CBR Debug] IfNode was empty, dropped!" << std::endl;
                        }
                        
                        continue;
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
