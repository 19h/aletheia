#include "car.hpp"
#include <vector>

namespace dewolf {

AstNode* ConditionAwareRefinement::refine(
    DecompilerArena& arena,
    z3::context& ctx,
    AstNode* root,
    const std::unordered_map<TransitionBlock*, dewolf_logic::LogicCondition>& reaching_conditions
) {
    if (auto* seq = dynamic_cast<SeqNode*>(root)) {
        // Condition Aware Refinement (CAR) port.
        // It converts sequences of `IfNodes` that share a variable comparison into a `SwitchNode`.
        
        std::vector<AstNode*> new_nodes;
        SwitchNode* active_switch = nullptr;

        for (AstNode* node : seq->nodes()) {
            if (IfNode* if_node = dynamic_cast<IfNode*>(node)) {
                // Determine if this IfNode checks equality for a potential switch
                if (ExprAstNode* expr_ast = dynamic_cast<ExprAstNode*>(if_node->cond())) {
                    // The condition might be a Condition (IS-A Operation) with eq type
                    if (auto* cond = dynamic_cast<Condition*>(expr_ast->expr())) {
                        if (cond->type() == OperationType::eq) {
                            if (auto* c = dynamic_cast<Constant*>(cond->rhs())) {
                                if (!active_switch) {
                                    active_switch = arena.create<SwitchNode>(arena.create<ExprAstNode>(cond->lhs()));
                                    new_nodes.push_back(active_switch);
                                }
                                CaseNode* case_n = arena.create<CaseNode>(c->value(), if_node->true_branch());
                                active_switch->add_case(case_n);
                                continue;
                            }
                        }
                    }
                    // Fallback: try generic Operation with eq type (legacy compatibility)
                    else if (auto* op = dynamic_cast<Operation*>(expr_ast->expr())) {
                        if (op->type() == OperationType::eq && op->operands().size() == 2) {
                            if (auto* c = dynamic_cast<Constant*>(op->operands()[1])) {
                                if (!active_switch) {
                                    active_switch = arena.create<SwitchNode>(arena.create<ExprAstNode>(op->operands()[0]));
                                    new_nodes.push_back(active_switch);
                                }
                                CaseNode* case_n = arena.create<CaseNode>(c->value(), if_node->true_branch());
                                active_switch->add_case(case_n);
                                continue;
                            }
                        }
                    }
                }
            }
            
            // If it's not a switchable if, close out the active switch
            active_switch = nullptr;
            new_nodes.push_back(node);
        }
        
        SeqNode* new_seq = arena.create<SeqNode>();
        for (auto* n : new_nodes) {
            new_seq->add_node(n);
        }
        return new_seq;
    }

    return root;
}

} // namespace dewolf
