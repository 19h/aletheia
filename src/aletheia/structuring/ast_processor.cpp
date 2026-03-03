#include <iostream>
#include <cstdlib>
#include "ast_processor.hpp"

namespace aletheia {

namespace {

bool same_if_condition(AstNode* lhs_cond, AstNode* rhs_cond) {
    if (lhs_cond == rhs_cond) {
        return true;
    }

    auto* lhs_expr = ast_dyn_cast<ExprAstNode>(lhs_cond);
    auto* rhs_expr = ast_dyn_cast<ExprAstNode>(rhs_cond);
    if (!lhs_expr || !rhs_expr || !lhs_expr->expr() || !rhs_expr->expr()) {
        return false;
    }

    return expression_fingerprint_hash(lhs_expr->expr()) == expression_fingerprint_hash(rhs_expr->expr());
}

enum class CompareDomain : std::uint8_t {
    Any,
    Signed,
    Unsigned,
};

enum class CompareFamily : std::uint8_t {
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Neq,
    Unknown,
};

struct CompareInfo {
    OperationType op = OperationType::unknown;
    Expression* lhs = nullptr;
    Expression* rhs = nullptr;
};

CompareDomain compare_domain(OperationType op) {
    switch (op) {
        case OperationType::lt:
        case OperationType::le:
        case OperationType::gt:
        case OperationType::ge:
            return CompareDomain::Signed;
        case OperationType::lt_us:
        case OperationType::le_us:
        case OperationType::gt_us:
        case OperationType::ge_us:
            return CompareDomain::Unsigned;
        case OperationType::eq:
        case OperationType::neq:
            return CompareDomain::Any;
        default:
            return CompareDomain::Any;
    }
}

CompareFamily compare_family(OperationType op) {
    switch (op) {
        case OperationType::lt:
        case OperationType::lt_us:
            return CompareFamily::Lt;
        case OperationType::le:
        case OperationType::le_us:
            return CompareFamily::Le;
        case OperationType::gt:
        case OperationType::gt_us:
            return CompareFamily::Gt;
        case OperationType::ge:
        case OperationType::ge_us:
            return CompareFamily::Ge;
        case OperationType::eq:
            return CompareFamily::Eq;
        case OperationType::neq:
            return CompareFamily::Neq;
        default:
            return CompareFamily::Unknown;
    }
}

OperationType flipped_compare(OperationType op) {
    switch (op) {
        case OperationType::lt: return OperationType::gt;
        case OperationType::le: return OperationType::ge;
        case OperationType::gt: return OperationType::lt;
        case OperationType::ge: return OperationType::le;
        case OperationType::lt_us: return OperationType::gt_us;
        case OperationType::le_us: return OperationType::ge_us;
        case OperationType::gt_us: return OperationType::lt_us;
        case OperationType::ge_us: return OperationType::le_us;
        case OperationType::eq:
        case OperationType::neq:
            return op;
        default:
            return OperationType::unknown;
    }
}

bool extract_compare(Expression* expr, CompareInfo& out) {
    if (!expr) {
        return false;
    }

    if (auto* cond = dyn_cast<Condition>(expr)) {
        if (compare_family(cond->type()) == CompareFamily::Unknown) {
            return false;
        }
        out.op = cond->type();
        out.lhs = cond->lhs();
        out.rhs = cond->rhs();
        return true;
    }

    auto* op = dyn_cast<Operation>(expr);
    if (!op || op->operands().size() != 2 || compare_family(op->type()) == CompareFamily::Unknown) {
        return false;
    }

    out.op = op->type();
    out.lhs = op->operands()[0];
    out.rhs = op->operands()[1];
    return true;
}

bool compare_ops_contradict(OperationType outer_op, OperationType inner_op) {
    const CompareFamily outer = compare_family(outer_op);
    const CompareFamily inner = compare_family(inner_op);
    if (outer == CompareFamily::Unknown || inner == CompareFamily::Unknown) {
        return false;
    }

    const CompareDomain outer_domain = compare_domain(outer_op);
    const CompareDomain inner_domain = compare_domain(inner_op);
    if (outer_domain != CompareDomain::Any && inner_domain != CompareDomain::Any && outer_domain != inner_domain) {
        return false;
    }

    switch (outer) {
        case CompareFamily::Lt:
            return inner == CompareFamily::Gt || inner == CompareFamily::Ge || inner == CompareFamily::Eq;
        case CompareFamily::Le:
            return inner == CompareFamily::Gt;
        case CompareFamily::Gt:
            return inner == CompareFamily::Lt || inner == CompareFamily::Le || inner == CompareFamily::Eq;
        case CompareFamily::Ge:
            return inner == CompareFamily::Lt;
        case CompareFamily::Eq:
            return inner == CompareFamily::Lt || inner == CompareFamily::Gt || inner == CompareFamily::Neq;
        case CompareFamily::Neq:
            return inner == CompareFamily::Eq;
        case CompareFamily::Unknown:
            return false;
    }
    return false;
}

bool conditions_contradict(Expression* outer_expr, Expression* inner_expr) {
    CompareInfo outer;
    CompareInfo inner;
    if (!extract_compare(outer_expr, outer) || !extract_compare(inner_expr, inner)) {
        return false;
    }

    const std::uint64_t outer_lhs_fp = expression_fingerprint_hash(outer.lhs);
    const std::uint64_t outer_rhs_fp = expression_fingerprint_hash(outer.rhs);
    const std::uint64_t inner_lhs_fp = expression_fingerprint_hash(inner.lhs);
    const std::uint64_t inner_rhs_fp = expression_fingerprint_hash(inner.rhs);

    OperationType aligned_inner_op = OperationType::unknown;
    if (outer_lhs_fp == inner_lhs_fp && outer_rhs_fp == inner_rhs_fp) {
        aligned_inner_op = inner.op;
    } else if (outer_lhs_fp == inner_rhs_fp && outer_rhs_fp == inner_lhs_fp) {
        aligned_inner_op = flipped_compare(inner.op);
    } else {
        return false;
    }

    if (aligned_inner_op == OperationType::unknown) {
        return false;
    }
    return compare_ops_contradict(outer.op, aligned_inner_op);
}

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

            if (!ifn->false_branch()) {
                if (auto* nested_if = ast_dyn_cast<IfNode>(ifn->true_branch())) {
                    if (!nested_if->false_branch() && same_if_condition(ifn->cond(), nested_if->cond())) {
                        ifn->set_true_branch(nested_if->true_branch());
                    }
                }
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

        if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
            auto* body_seq = ast_dyn_cast<SeqNode>(loop->body());
            if (!loop->condition() || !body_seq || body_seq->empty()) {
                return loop;
            }

            auto* lead_if = ast_dyn_cast<IfNode>(body_seq->first());
            if (!lead_if || !lead_if->is_break_condition()) {
                return loop;
            }

            auto* cond_ast = ast_dyn_cast<ExprAstNode>(lead_if->cond());
            if (!cond_ast || !cond_ast->expr()) {
                return loop;
            }

            if (conditions_contradict(loop->condition(), cond_ast->expr())) {
                auto& nodes = body_seq->mutable_nodes();
                nodes.erase(nodes.begin());
            }
            return loop;
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
