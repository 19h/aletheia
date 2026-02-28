#include "loop_structurer.hpp"
#include <algorithm>
#include <unordered_set>

namespace aletheia {

// =============================================================================
// Helpers
// =============================================================================

/// Negate a Condition expression: if it's a Condition (comparison), flip the
/// comparison operator. Otherwise, wrap in logical_not.
static Expression* negate_condition_expr(DecompilerArena& arena, Expression* expr) {
    if (auto* cond = dynamic_cast<Condition*>(expr)) {
        auto negated_op = Condition::negate_comparison(cond->type());
        return arena.create<Condition>(negated_op, cond->lhs(), cond->rhs(), cond->size_bytes);
    }
    // Fallback: wrap in logical_not
    return arena.create<Operation>(
        OperationType::logical_not, std::vector<Expression*>{expr}, 1);
}

/// Extract the break condition's Expression* from an IfNode that is a
/// break-condition (single-branch if whose true arm is a break-only CodeNode).
/// Returns nullptr if not a break-condition.
static Expression* extract_break_condition_expr(AstNode* node) {
    auto* ifn = dynamic_cast<IfNode*>(node);
    if (!ifn) return nullptr;
    if (ifn->false_branch() != nullptr) return nullptr;
    if (!ifn->true_branch() || !ifn->true_branch()->is_break_node()) return nullptr;
    return ifn->condition_expr();
}

/// Check that all code nodes interrupting the ancestor loop are in the
/// given end_nodes set.
static bool has_only_loop_interruptions_in(
    const std::unordered_set<AstNode*>& end_nodes, SeqNode* body) {
    std::vector<CodeNode*> interrupting;
    body->get_descendant_code_nodes_interrupting_ancestor_loop(interrupting);
    for (auto* cn : interrupting) {
        if (!end_nodes.contains(static_cast<AstNode*>(cn))) return false;
    }
    return true;
}

/// Remove all break statements from end-nodes of a subtree.
static void delete_break_statements(AstNode* node) {
    std::vector<AstNode*> ends;
    node->get_end_nodes(ends);
    for (auto* end : ends) {
        if (auto* cn = dynamic_cast<CodeNode*>(end)) {
            cn->clean();
            if (cn->does_end_with_break()) {
                cn->remove_last_instruction();
            }
        }
    }
}

// =============================================================================
// WhileLoopRule
// =============================================================================

bool WhileLoopRule::can_be_applied(LoopNode* loop) const {
    if (!loop->is_endless()) return false;
    AstNode* body = loop->body();
    if (!body) return false;

    // Case 1: body itself is a break-condition
    if (body->is_break_condition()) return true;

    // Case 2: body is a SeqNode whose first child is a break-condition
    if (auto* seq = dynamic_cast<SeqNode*>(body)) {
        if (!seq->nodes().empty() && seq->first()->is_break_condition()) return true;
    }
    return false;
}

AstNode* WhileLoopRule::restructure(DecompilerArena& arena, LoopNode* loop) {
    AstNode* body = loop->body();

    // Case 1: body IS the break-condition
    // while(true) { if(cond) break; } -> while(!cond) {}
    if (body->is_break_condition()) {
        Expression* break_cond = extract_break_condition_expr(body);
        Expression* negated = negate_condition_expr(arena, break_cond);
        loop->set_condition(negated);
        // Remove the break-condition from body -> body becomes empty
        loop->set_body(nullptr);
        return loop;
    }

    // Case 2: body is SeqNode, first child is break-condition
    auto* seq = dynamic_cast<SeqNode*>(body);
    AstNode* first = seq->first();
    Expression* break_cond = extract_break_condition_expr(first);
    Expression* negated = negate_condition_expr(arena, break_cond);
    loop->set_condition(negated);

    // Remove the first child (the break-condition) from the sequence
    seq->remove_node(first);

    // If only one child remains, unwrap the SeqNode
    if (seq->size() == 1) {
        loop->set_body(seq->first());
    }

    return loop;
}

// =============================================================================
// DoWhileLoopRule
// =============================================================================

bool DoWhileLoopRule::can_be_applied(LoopNode* loop) const {
    if (!loop->is_endless()) return false;
    auto* seq = dynamic_cast<SeqNode*>(loop->body());
    if (!seq || seq->empty()) return false;
    return seq->last()->is_break_condition();
}

AstNode* DoWhileLoopRule::restructure(DecompilerArena& arena, LoopNode* loop) {
    auto* seq = dynamic_cast<SeqNode*>(loop->body());
    AstNode* last = seq->last();

    Expression* break_cond = extract_break_condition_expr(last);
    Expression* negated = negate_condition_expr(arena, break_cond);

    // Remove the last child (break-condition) from the sequence
    seq->remove_node(last);

    // Build the do-while body
    AstNode* new_body = seq;
    if (seq->size() == 1) new_body = seq->first();

    // Create a DoWhileLoopNode
    auto* dowhile = arena.create<DoWhileLoopNode>(new_body, negated);
    return dowhile;
}

// =============================================================================
// NestedDoWhileLoopRule
// =============================================================================

bool NestedDoWhileLoopRule::can_be_applied(LoopNode* loop) const {
    if (!loop->is_endless()) return false;
    auto* seq = dynamic_cast<SeqNode*>(loop->body());
    if (!seq || seq->empty()) return false;

    // Last child must be a single-branch condition node
    if (!seq->last()->is_single_branch()) return false;

    // No other child (before the last) may have code nodes that break
    // the ancestor loop
    auto& nodes = seq->nodes();
    for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
        if (nodes[i]->has_descendant_code_node_breaking_ancestor_loop())
            return false;
    }
    return true;
}

AstNode* NestedDoWhileLoopRule::restructure(DecompilerArena& arena, LoopNode* loop) {
    auto* seq = dynamic_cast<SeqNode*>(loop->body());
    auto& nodes = seq->mutable_nodes();

    // Last node is a single-branch ConditionNode (IfNode)
    auto* last_if = dynamic_cast<IfNode*>(nodes.back());
    Expression* cond_expr = last_if->condition_expr();
    Expression* negated = negate_condition_expr(arena, cond_expr);

    // The single branch's body becomes a sibling after the do-while
    AstNode* branch_body = last_if->true_branch();

    // Remove the last node from the sequence
    nodes.pop_back();

    // Build the do-while body from remaining children
    AstNode* dowhile_body = seq;
    if (seq->size() == 1) dowhile_body = seq->first();

    auto* dowhile = arena.create<DoWhileLoopNode>(dowhile_body, negated);

    // Create a sequence: [do-while, branch_body]
    auto* result_seq = arena.create<SeqNode>();
    result_seq->add_node(dowhile);
    if (branch_body) result_seq->add_node(branch_body);

    return result_seq;
}

// =============================================================================
// SequenceRule
// =============================================================================

bool SequenceRule::can_be_applied(LoopNode* loop) const {
    if (!loop->is_endless()) return false;
    auto* seq = dynamic_cast<SeqNode*>(loop->body());
    if (!seq) return false;

    // All end-nodes must be code-nodes ending with break
    std::vector<AstNode*> ends;
    seq->get_end_nodes(ends);
    if (ends.empty()) return false;

    std::unordered_set<AstNode*> end_set;
    for (auto* end : ends) {
        if (!end->is_code_node_ending_with_break()) return false;
        end_set.insert(end);
    }

    // No other break/continue interruptions allowed
    return has_only_loop_interruptions_in(end_set, seq);
}

AstNode* SequenceRule::restructure(DecompilerArena& arena, LoopNode* loop) {
    (void)arena;
    AstNode* body = loop->body();

    // Remove all break statements from end-nodes
    delete_break_statements(body);

    // The loop was not really a loop -- return the body directly
    return body;
}

// =============================================================================
// ConditionToSequenceRule
// =============================================================================

bool ConditionToSequenceRule::can_be_applied(LoopNode* loop) const {
    if (!loop->is_endless()) return false;
    auto* ifn = dynamic_cast<IfNode*>(loop->body());
    if (!ifn) return false;

    bool break_in_true = ifn->true_branch() ? ifn->true_branch()->does_contain_break() : false;
    bool break_in_false = ifn->false_branch() ? ifn->false_branch()->does_contain_break() : false;

    // Exactly one branch must contain breaks (XOR)
    return break_in_true != break_in_false;
}

AstNode* ConditionToSequenceRule::restructure(DecompilerArena& arena, LoopNode* loop) {
    auto* ifn = dynamic_cast<IfNode*>(loop->body());

    bool break_in_true = ifn->true_branch() ? ifn->true_branch()->does_contain_break() : false;

    // If the true branch has breaks, swap branches so the false branch
    // has the breaks (the break branch becomes the suffix after the loop).
    if (break_in_true) {
        ifn->switch_branches();
    }

    // Now: false_branch has breaks (suffix), true_branch is the loop body
    AstNode* new_loop_body = ifn->true_branch();
    AstNode* suffix = ifn->false_branch();

    // Get the condition for the new while loop
    Expression* cond_expr = ifn->condition_expr();
    // If we swapped branches, the condition is already correct (we want to
    // loop while the original true branch's condition holds). If we didn't
    // swap, the condition needs to be negated (we loop while NOT the
    // false condition).
    if (!break_in_true && cond_expr) {
        cond_expr = negate_condition_expr(arena, cond_expr);
    }

    // Remove break statements from the suffix
    if (suffix) delete_break_statements(suffix);

    // Create the new while loop with condition
    auto* new_loop = arena.create<WhileLoopNode>(new_loop_body, cond_expr);

    // Create sequence: [while(cond) { body }, suffix]
    auto* result = arena.create<SeqNode>();
    result->add_node(new_loop);
    if (suffix) result->add_node(suffix);

    return result;
}

// =============================================================================
// LoopStructurer -- Orchestrator
// =============================================================================

// Static rule instances
WhileLoopRule LoopStructurer::while_rule_;
DoWhileLoopRule LoopStructurer::do_while_rule_;
NestedDoWhileLoopRule LoopStructurer::nested_do_while_rule_;
SequenceRule LoopStructurer::sequence_rule_;
ConditionToSequenceRule LoopStructurer::condition_to_sequence_rule_;

LoopStructuringRule* LoopStructurer::match_rule(LoopNode* loop) {
    if (!loop->is_endless()) return nullptr;

    // Try rules in priority order (matching the Python reference)
    if (while_rule_.can_be_applied(loop)) return &while_rule_;
    if (do_while_rule_.can_be_applied(loop)) return &do_while_rule_;
    if (nested_do_while_rule_.can_be_applied(loop)) return &nested_do_while_rule_;
    if (sequence_rule_.can_be_applied(loop)) return &sequence_rule_;
    if (condition_to_sequence_rule_.can_be_applied(loop)) return &condition_to_sequence_rule_;

    return nullptr;
}

AstNode* LoopStructurer::refine_loop(DecompilerArena& arena, LoopNode* loop) {
    AstNode* current = loop;

    // Iterate: apply a matching rule, then re-check. The rule may produce
    // a new root that is still a loop needing further refinement.
    while (auto* loop_node = dynamic_cast<LoopNode*>(current)) {
        if (!loop_node->is_endless()) break; // Already has a condition, done

        auto* rule = match_rule(loop_node);
        if (!rule) break; // No rule matches, done

        current = rule->restructure(arena, loop_node);
    }

    return current;
}

} // namespace aletheia
