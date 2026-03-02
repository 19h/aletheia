#pragma once
#include "ast.hpp"
#include "../../common/arena.hpp"

namespace aletheia {

class AstProcessor {
public:
    static AstNode* preprocess_loop(DecompilerArena& arena, AstNode* root);
    static AstNode* postprocess_loop(DecompilerArena& arena, AstNode* root);
    static AstNode* preprocess_acyclic(DecompilerArena& arena, AstNode* root);
    static AstNode* postprocess_acyclic(DecompilerArena& arena, AstNode* root);

    // Helpers exposed for testing/reuse
    static Expression* negate_condition_expr(DecompilerArena& arena, Expression* expr);
    static void switch_branches(DecompilerArena& arena, IfNode* ifn);
    static AstNode* clean_node(DecompilerArena& arena, AstNode* node);

private:
    static AstNode* combine_cascading_breaks(DecompilerArena& arena, AstNode* root);
    static AstNode* extract_conditional_breaks(DecompilerArena& arena, AstNode* root);
    static AstNode* extract_conditional_continues(DecompilerArena& arena, AstNode* root);
    static AstNode* extract_conditional_returns(DecompilerArena& arena, AstNode* root);
    static AstNode* remove_redundant_continues(DecompilerArena& arena, AstNode* root);
    static AstNode* remove_redundant_continue_at_end_of_sequence(DecompilerArena& arena, AstNode* root);
    static AstNode* sort_sequence_node_children_while_over_do_while(DecompilerArena& arena, AstNode* root);
};

} // namespace aletheia
