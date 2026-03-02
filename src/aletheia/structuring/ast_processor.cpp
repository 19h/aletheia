#include <iostream>
#include <cstdlib>
#include "ast_processor.hpp"

namespace aletheia {

namespace {

template<typename Func>
AstNode* rewrite_ast(DecompilerArena& arena, AstNode* node, Func f) {
    if (!node) return nullptr;

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        std::vector<AstNode*> new_children;
        for (auto* child : seq->nodes()) {
            if (AstNode* rewritten = rewrite_ast(arena, child, f)) {
                new_children.push_back(rewritten);
            }
        }
        seq->mutable_nodes() = std::move(new_children);
    } else if (auto* ifn = ast_dyn_cast<IfNode>(node)) {
        ifn->set_true_branch(rewrite_ast(arena, ifn->true_branch(), f));
        ifn->set_false_branch(rewrite_ast(arena, ifn->false_branch(), f));
    } else if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        loop->set_body(rewrite_ast(arena, loop->body(), f));
    } else if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (auto* case_node : sw->cases()) {
            if (auto* c = ast_dyn_cast<CaseNode>(case_node)) {
                c->set_body(rewrite_ast(arena, c->body(), f));
            }
        }
    }

    return f(node);
}

bool is_empty_node(AstNode* node) {
    if (!node) return true;
    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        return seq->empty();
    }
    if (auto* code = ast_dyn_cast<CodeNode>(node)) {
        return code->block() && code->block()->instructions().empty();
    }
    return false;
}

template<typename Predicate>
AstNode* extract_conditional_interruption(DecompilerArena& arena, AstNode* node, Predicate is_interruption) {
    auto* ifn = ast_dyn_cast<IfNode>(node);
    if (!ifn) return node;

    bool t_ends = ifn->true_branch() && is_interruption(ifn->true_branch());
    bool f_ends = ifn->false_branch() && is_interruption(ifn->false_branch());


    if (getenv("DEBUG_EXTRACT")) {
        std::cerr << "extract_conditional_interruption visited IfNode.\n";
        std::cerr << "  t_ends=" << t_ends << " f_ends=" << f_ends << "\n";
        if (ifn->true_branch()) {
            std::cerr << "  true_branch ends_with_return? " << ifn->true_branch()->does_end_with_return() << "\n";
            std::cerr << "  true_branch ends_with_break? " << ifn->true_branch()->does_end_with_break() << "\n";
            std::cerr << "  is_interruption(true_branch)? " << is_interruption(ifn->true_branch()) << "\n";
        }
    }
    if (t_ends == f_ends) {
        return node;
    }

    bool extract_true = !t_ends;
    AstNode* extracted = extract_true ? ifn->true_branch() : ifn->false_branch();
    
    if (extract_true) {
        ifn->set_true_branch(nullptr);
    } else {
        ifn->set_false_branch(nullptr);
    }

    AstNode* cleaned_ifn = AstProcessor::clean_node(arena, ifn);

    if (!extracted) return cleaned_ifn;

    auto* seq = arena.create<SeqNode>();
    if (cleaned_ifn) seq->add_node(cleaned_ifn);
    seq->add_node(extracted);
    return AstProcessor::clean_node(arena, seq);
}

} // namespace

Expression* AstProcessor::negate_condition_expr(DecompilerArena& arena, Expression* expr) {
    if (!expr) return nullptr;
    if (auto* cond = dyn_cast<Condition>(expr)) {
        auto negated_op = Condition::negate_comparison(cond->type());
        return arena.create<Condition>(negated_op, cond->lhs(), cond->rhs(), cond->size_bytes);
    }
    return arena.create<Operation>(
        OperationType::logical_not, std::vector<Expression*>{expr}, 1);
}

void AstProcessor::switch_branches(DecompilerArena& arena, IfNode* ifn) {
    AstNode* t = ifn->true_branch();
    AstNode* f = ifn->false_branch();
    ifn->set_true_branch(f);
    ifn->set_false_branch(t);

    if (auto* expr_ast = ast_dyn_cast<ExprAstNode>(ifn->cond())) {
        Expression* negated = negate_condition_expr(arena, expr_ast->expr());
        ifn->set_cond(arena.create<ExprAstNode>(negated));
    }
}

AstNode* AstProcessor::clean_node(DecompilerArena& arena, AstNode* root) {
    return rewrite_ast(arena, root, [&](AstNode* node) -> AstNode* {
        if (auto* code = ast_dyn_cast<CodeNode>(node)) {
            code->clean();
            if (is_empty_node(code)) return nullptr;
            return code;
        }

        if (auto* ifn = ast_dyn_cast<IfNode>(node)) {
            if (is_empty_node(ifn->true_branch())) ifn->set_true_branch(nullptr);
            if (is_empty_node(ifn->false_branch())) ifn->set_false_branch(nullptr);

            if (!ifn->true_branch() && ifn->false_branch()) {
                switch_branches(arena, ifn);
            }
            if (!ifn->true_branch() && !ifn->false_branch()) return nullptr;
            return ifn;
        }

        if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
            std::vector<AstNode*> flattened;
            for (auto* child : seq->nodes()) {
                if (auto* child_seq = ast_dyn_cast<SeqNode>(child)) {
                    for (auto* c : child_seq->nodes()) flattened.push_back(c);
                } else if (!is_empty_node(child)) {
                    flattened.push_back(child);
                }
            }
            seq->mutable_nodes() = flattened;
            if (seq->empty()) return nullptr;
            if (seq->size() == 1) return seq->first();
            return seq;
        }

        return node;
    });
}

AstNode* AstProcessor::extract_conditional_breaks(DecompilerArena& arena, AstNode* root) {
    return rewrite_ast(arena, root, [&](AstNode* node) -> AstNode* {
        return extract_conditional_interruption(arena, node, [](AstNode* n) {
            return n->does_end_with_break();
        });
    });
}

AstNode* AstProcessor::extract_conditional_continues(DecompilerArena& arena, AstNode* root) {
    return rewrite_ast(arena, root, [&](AstNode* node) -> AstNode* {
        return extract_conditional_interruption(arena, node, [](AstNode* n) {
            return n->does_end_with_continue();
        });
    });
}

AstNode* AstProcessor::extract_conditional_returns(DecompilerArena& arena, AstNode* root) {
    return rewrite_ast(arena, root, [&](AstNode* node) -> AstNode* {
        return extract_conditional_interruption(arena, node, [](AstNode* n) {
            return n->does_end_with_return();
        });
    });
}

AstNode* AstProcessor::remove_redundant_continues(DecompilerArena& arena, AstNode* root) {
    return rewrite_ast(arena, root, [&](AstNode* node) -> AstNode* {
        if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
            if (loop->body()) {
                std::vector<AstNode*> ends;
                loop->body()->get_end_nodes(ends);
                for (AstNode* end : ends) {
                    if (auto* code = ast_dyn_cast<CodeNode>(end)) {
                        if (code->does_end_with_continue()) {
                            code->remove_last_instruction();
                        }
                    }
                }
            }
        }
        return node;
    });
}

AstNode* AstProcessor::remove_redundant_continue_at_end_of_sequence(DecompilerArena& arena, AstNode* root) {
    return rewrite_ast(arena, root, [&](AstNode* node) -> AstNode* {
        if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
            if (loop->is_endless() && loop->body()) {
                if (auto* seq = ast_dyn_cast<SeqNode>(loop->body())) {
                    if (!seq->empty()) {
                        if (auto* code = ast_dyn_cast<CodeNode>(seq->last())) {
                            if (code->does_end_with_continue()) {
                                code->remove_last_instruction();
                            }
                        }
                    }
                }
            }
        }
        return node;
    });
}

AstNode* AstProcessor::combine_cascading_breaks(DecompilerArena& arena, AstNode* root) {
    return rewrite_ast(arena, root, [&](AstNode* node) -> AstNode* {
        if (auto* ifn = ast_dyn_cast<IfNode>(node)) {
            // Check if BOTH branches eventually end with a break.
            // If they do, and neither does anything else, we can merge them.
            // A more robust check: does_end_with_break() on both.
            if (ifn->true_branch() && ifn->true_branch()->does_end_with_break() &&
                ifn->false_branch() && ifn->false_branch()->does_end_with_break()) {
                
                // If they are pure breaks (or break conditions), we replace with a BreakNode.
                // For simplicity, we just return the true_branch which is a break.
                // But wait, if they contain logic, we can't just drop it.
                // Let's just create a generic BreakNode and rely on CodeVisitor to ignore it if unhandled,
                // BUT CodeVisitor doesn't handle BreakNode. So we should return the true branch if it's purely a break.
                if (ifn->true_branch()->is_break_node() && ifn->false_branch()->is_break_node()) {
                    return ifn->true_branch();
                }
            }
        }
        return node;
    });
}

AstNode* AstProcessor::sort_sequence_node_children_while_over_do_while(DecompilerArena& arena, AstNode* root) {
    return rewrite_ast(arena, root, [&](AstNode* node) -> AstNode* {
        if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
            if (seq->empty()) return seq;
            
            auto& children = seq->mutable_nodes();
            for (auto it = children.begin(); it != children.end(); ++it) {
                if (auto* ifn = ast_dyn_cast<IfNode>(*it)) {
                    // Check if it's a break condition: single branch, ending with break
                    if (ifn->true_branch() && ifn->true_branch()->does_end_with_break() && !ifn->false_branch()) {
                        // Float it to the front
                        AstNode* break_cond = *it;
                        children.erase(it);
                        children.insert(children.begin(), break_cond);
                        break;
                    }
                }
            }
        }
        return node;
    });
}

AstNode* AstProcessor::preprocess_loop(DecompilerArena& arena, AstNode* root) {
    root = clean_node(arena, root);
    root = combine_cascading_breaks(arena, root);
    root = extract_conditional_breaks(arena, root);
    root = clean_node(arena, root);
    root = remove_redundant_continue_at_end_of_sequence(arena, root);
    return clean_node(arena, root);
}

AstNode* AstProcessor::postprocess_loop(DecompilerArena& arena, AstNode* root) {
    root = clean_node(arena, root);
    root = extract_conditional_continues(arena, root);
    root = remove_redundant_continues(arena, root);
    return clean_node(arena, root);
}

AstNode* AstProcessor::preprocess_acyclic(DecompilerArena& arena, AstNode* root) {
    return clean_node(arena, root);
}

AstNode* AstProcessor::postprocess_acyclic(DecompilerArena& arena, AstNode* root) {
    if (getenv("DEBUG_EXTRACT")) {
        std::cerr << "postprocess_acyclic called\n";
    }
    root = clean_node(arena, root);
    root = combine_cascading_breaks(arena, root);
    root = extract_conditional_breaks(arena, root);
    root = extract_conditional_returns(arena, root);
    root = sort_sequence_node_children_while_over_do_while(arena, root);
    return clean_node(arena, root);
}

} // namespace aletheia
