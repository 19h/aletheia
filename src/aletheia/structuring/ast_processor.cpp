#include <iostream>
#include <cstdlib>
#include <optional>
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

std::string variable_key(const Variable* v) {
    if (!v) {
        return "";
    }
    return v->name() + "#" + std::to_string(v->ssa_version());
}

void collect_condition_variable_keys(Expression* expr, std::unordered_set<std::string>& keys) {
    if (!expr) {
        return;
    }
    std::unordered_set<Variable*> required;
    expr->collect_requirements(required);
    for (Variable* var : required) {
        if (var) {
            keys.insert(variable_key(var));
        }
    }
}

bool ast_writes_any_condition_variable(AstNode* node, const std::unordered_set<std::string>& keys) {
    if (!node || keys.empty()) {
        return false;
    }

    if (auto* code = ast_dyn_cast<CodeNode>(node)) {
        if (!code->block()) {
            return false;
        }
        for (Instruction* inst : code->block()->instructions()) {
            auto* assign = dyn_cast<Assignment>(inst);
            auto* dst = assign ? dyn_cast<Variable>(assign->destination()) : nullptr;
            if (dst && keys.contains(variable_key(dst))) {
                return true;
            }
        }
        return false;
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode* child : seq->nodes()) {
            if (ast_writes_any_condition_variable(child, keys)) {
                return true;
            }
        }
        return false;
    }

    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        return ast_writes_any_condition_variable(if_node->true_branch(), keys)
            || ast_writes_any_condition_variable(if_node->false_branch(), keys);
    }

    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        return ast_writes_any_condition_variable(loop->body(), keys);
    }

    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* case_node : sw->cases()) {
            if (ast_writes_any_condition_variable(case_node->body(), keys)) {
                return true;
            }
        }
        return false;
    }

    if (auto* case_node = ast_dyn_cast<CaseNode>(node)) {
        return ast_writes_any_condition_variable(case_node->body(), keys);
    }

    return false;
}

AstNode* strip_terminal_flow(AstNode* node, bool strip_continue) {
    if (!node) {
        return nullptr;
    }

    if (auto* code = ast_dyn_cast<CodeNode>(node)) {
        if ((strip_continue && code->does_end_with_continue()) ||
            (!strip_continue && code->does_end_with_break())) {
            code->remove_last_instruction();
        }
        if (code->block() && code->block()->instructions().empty()) {
            return nullptr;
        }
        return code;
    }

    auto* seq = ast_dyn_cast<SeqNode>(node);
    if (!seq || seq->empty()) {
        return node;
    }

    AstNode* stripped_tail = strip_terminal_flow(seq->last(), strip_continue);
    if (!stripped_tail) {
        seq->mutable_nodes().pop_back();
    } else {
        seq->mutable_nodes().back() = stripped_tail;
    }

    if (seq->empty()) {
        return nullptr;
    }
    if (seq->size() == 1) {
        return seq->first();
    }
    return seq;
}

template<typename Func>
AstNode* rewrite_ast(DecompilerArena& arena, AstNode* node, Func f);

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

struct CompareWithConstant {
    OperationType op = OperationType::unknown;
    Expression* expr = nullptr;
    Constant* constant = nullptr;
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

bool normalize_compare_with_constant(const CompareInfo& in, CompareWithConstant& out) {
    auto* lhs_const = dyn_cast<Constant>(in.lhs);
    auto* rhs_const = dyn_cast<Constant>(in.rhs);

    if (lhs_const && !rhs_const) {
        out.op = flipped_compare(in.op);
        out.expr = in.rhs;
        out.constant = lhs_const;
        return out.op != OperationType::unknown;
    }
    if (rhs_const && !lhs_const) {
        out.op = in.op;
        out.expr = in.lhs;
        out.constant = rhs_const;
        return out.op != OperationType::unknown;
    }
    return false;
}

std::optional<bool> compare_with_constant_implication(const CompareWithConstant& outer, const CompareWithConstant& inner) {
    if (!outer.expr || !inner.expr || !outer.constant || !inner.constant) {
        return std::nullopt;
    }
    if (expression_fingerprint_hash(outer.expr) != expression_fingerprint_hash(inner.expr)) {
        return std::nullopt;
    }

    const std::uint64_t outer_value = outer.constant->value();
    const std::uint64_t inner_value = inner.constant->value();

    if (outer.op == OperationType::eq) {
        if (inner.op == OperationType::eq) {
            return outer_value == inner_value;
        }
        if (inner.op == OperationType::neq) {
            return outer_value != inner_value;
        }
    }

    if (outer.op == OperationType::neq && outer_value == inner_value) {
        if (inner.op == OperationType::eq) {
            return false;
        }
        if (inner.op == OperationType::neq) {
            return true;
        }
    }

    return std::nullopt;
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

std::optional<bool> compare_ops_implication(OperationType outer_op, OperationType inner_op) {
    const CompareFamily outer = compare_family(outer_op);
    const CompareFamily inner = compare_family(inner_op);
    if (outer == CompareFamily::Unknown || inner == CompareFamily::Unknown) {
        return std::nullopt;
    }

    const CompareDomain outer_domain = compare_domain(outer_op);
    const CompareDomain inner_domain = compare_domain(inner_op);
    if (outer_domain != CompareDomain::Any && inner_domain != CompareDomain::Any && outer_domain != inner_domain) {
        return std::nullopt;
    }

    switch (outer) {
        case CompareFamily::Eq:
            switch (inner) {
                case CompareFamily::Eq:
                case CompareFamily::Le:
                case CompareFamily::Ge:
                    return true;
                case CompareFamily::Neq:
                case CompareFamily::Lt:
                case CompareFamily::Gt:
                    return false;
                case CompareFamily::Unknown:
                    return std::nullopt;
            }
            break;
        case CompareFamily::Neq:
            switch (inner) {
                case CompareFamily::Neq:
                    return true;
                case CompareFamily::Eq:
                    return false;
                default:
                    return std::nullopt;
            }
        case CompareFamily::Lt:
            switch (inner) {
                case CompareFamily::Lt:
                case CompareFamily::Le:
                case CompareFamily::Neq:
                    return true;
                case CompareFamily::Eq:
                case CompareFamily::Ge:
                    return false;
                default:
                    return std::nullopt;
            }
        case CompareFamily::Le:
            switch (inner) {
                case CompareFamily::Le:
                    return true;
                case CompareFamily::Gt:
                    return false;
                default:
                    return std::nullopt;
            }
        case CompareFamily::Gt:
            switch (inner) {
                case CompareFamily::Gt:
                case CompareFamily::Ge:
                case CompareFamily::Neq:
                    return true;
                case CompareFamily::Eq:
                case CompareFamily::Le:
                    return false;
                default:
                    return std::nullopt;
            }
        case CompareFamily::Ge:
            switch (inner) {
                case CompareFamily::Ge:
                    return true;
                case CompareFamily::Lt:
                    return false;
                default:
                    return std::nullopt;
            }
        case CompareFamily::Unknown:
            return std::nullopt;
    }

    return std::nullopt;
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

std::optional<bool> condition_truth_under_outer(Expression* outer_expr, Expression* inner_expr) {
    CompareInfo outer;
    CompareInfo inner;
    if (!extract_compare(outer_expr, outer) || !extract_compare(inner_expr, inner)) {
        return std::nullopt;
    }

    CompareWithConstant outer_const;
    CompareWithConstant inner_const;
    if (normalize_compare_with_constant(outer, outer_const) &&
        normalize_compare_with_constant(inner, inner_const)) {
        std::optional<bool> implied = compare_with_constant_implication(outer_const, inner_const);
        if (implied.has_value()) {
            return implied;
        }
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
        return std::nullopt;
    }

    if (aligned_inner_op == OperationType::unknown) {
        return std::nullopt;
    }
    return compare_ops_implication(outer.op, aligned_inner_op);
}

AstNode* simplify_branch_with_outer_condition(
    DecompilerArena& arena,
    AstNode* node,
    Expression* outer_condition
) {
    if (!node || !outer_condition) {
        return node;
    }

    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        if (auto* cond_ast = ast_dyn_cast<ExprAstNode>(if_node->cond()); cond_ast && cond_ast->expr()) {
            std::optional<bool> implied =
                condition_truth_under_outer(outer_condition, cond_ast->expr());
            if (implied.has_value()) {
                AstNode* chosen = implied.value() ? if_node->true_branch() : if_node->false_branch();
                if (!chosen) {
                    return nullptr;
                }
                return simplify_branch_with_outer_condition(arena, chosen, outer_condition);
            }
        }

        if_node->set_true_branch(simplify_branch_with_outer_condition(arena, if_node->true_branch(), outer_condition));
        if_node->set_false_branch(simplify_branch_with_outer_condition(arena, if_node->false_branch(), outer_condition));
        if (!if_node->true_branch() && !if_node->false_branch()) {
            return nullptr;
        }
        return if_node;
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        std::vector<AstNode*> rewritten;
        rewritten.reserve(seq->size());
        for (AstNode* child : seq->nodes()) {
            AstNode* reduced = simplify_branch_with_outer_condition(arena, child, outer_condition);
            if (!reduced) {
                continue;
            }
            if (auto* child_seq = ast_dyn_cast<SeqNode>(reduced)) {
                for (AstNode* nested : child_seq->nodes()) {
                    if (nested) {
                        rewritten.push_back(nested);
                    }
                }
            } else {
                rewritten.push_back(reduced);
            }
        }
        seq->mutable_nodes() = std::move(rewritten);
        if (seq->empty()) {
            return nullptr;
        }
        if (seq->size() == 1) {
            return seq->first();
        }
        return seq;
    }

    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        loop->set_body(simplify_branch_with_outer_condition(arena, loop->body(), outer_condition));
        return loop;
    }

    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* case_node : sw->mutable_cases()) {
            case_node->set_body(simplify_branch_with_outer_condition(arena, case_node->body(), outer_condition));
        }
        return sw;
    }

    if (auto* case_node = ast_dyn_cast<CaseNode>(node)) {
        case_node->set_body(simplify_branch_with_outer_condition(arena, case_node->body(), outer_condition));
        return case_node;
    }

    return node;
}

AstNode* canonicalize_tail_induction_loops(DecompilerArena& arena, AstNode* root) {
    return rewrite_ast(arena, root, [&](AstNode* node) -> AstNode* {
        auto* loop = ast_dyn_cast<WhileLoopNode>(node);
        if (!loop || !loop->is_endless() || !loop->body()) {
            return node;
        }

        auto* body_seq = ast_dyn_cast<SeqNode>(loop->body());
        if (!body_seq || body_seq->size() < 2) {
            return node;
        }

        auto& body_nodes = body_seq->mutable_nodes();
        AstNode* tail_break = body_nodes.back();
        auto* tail_if = ast_dyn_cast<IfNode>(body_nodes[body_nodes.size() - 2]);
        if (!tail_if || tail_if->false_branch() != nullptr) {
            return node;
        }
        if (!tail_if->true_branch() || !tail_if->true_branch()->does_end_with_continue()) {
            return node;
        }
        if (!tail_break->does_end_with_break()) {
            return node;
        }

        auto* cond_ast = ast_dyn_cast<ExprAstNode>(tail_if->cond());
        Expression* cond_expr = cond_ast ? cond_ast->expr() : nullptr;
        if (!cond_expr) {
            return node;
        }

        std::unordered_set<std::string> cond_keys;
        collect_condition_variable_keys(cond_expr, cond_keys);
        if (cond_keys.empty() || !ast_writes_any_condition_variable(body_seq, cond_keys)) {
            return node;
        }

        AstNode* true_body = strip_terminal_flow(tail_if->true_branch(), /*strip_continue=*/true);
        if (!true_body) {
            return node;
        }
        AstNode* false_body = strip_terminal_flow(tail_break, /*strip_continue=*/false);

        AstNode* replacement_tail = arena.create<IfNode>(tail_if->cond(), true_body, false_body);
        body_nodes[body_nodes.size() - 2] = replacement_tail;
        body_nodes.pop_back();

        AstNode* do_body = body_seq;
        if (body_seq->size() == 1) {
            do_body = body_seq->first();
        }
        return arena.create<DoWhileLoopNode>(do_body, cond_expr);
    });
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

            if (auto* outer_cond_ast = ast_dyn_cast<ExprAstNode>(ifn->cond());
                outer_cond_ast && outer_cond_ast->expr()) {
                ifn->set_true_branch(
                    simplify_branch_with_outer_condition(arena, ifn->true_branch(), outer_cond_ast->expr()));
            }

            if (!ifn->false_branch()) {
                if (auto* nested_if = ast_dyn_cast<IfNode>(ifn->true_branch())) {
                    if (!nested_if->false_branch() && same_if_condition(ifn->cond(), nested_if->cond())) {
                        ifn->set_true_branch(nested_if->true_branch());
                    } else if (auto* outer_cond_ast = ast_dyn_cast<ExprAstNode>(ifn->cond());
                               outer_cond_ast && outer_cond_ast->expr()) {
                        if (auto* inner_cond_ast = ast_dyn_cast<ExprAstNode>(nested_if->cond());
                            inner_cond_ast && inner_cond_ast->expr()) {
                            std::optional<bool> implied =
                                condition_truth_under_outer(outer_cond_ast->expr(), inner_cond_ast->expr());
                            if (implied.has_value()) {
                                if (implied.value()) {
                                    ifn->set_true_branch(nested_if->true_branch());
                                } else {
                                    ifn->set_true_branch(nested_if->false_branch());
                                }
                            }
                        }
                    }
                }

                if (auto* true_seq = ast_dyn_cast<SeqNode>(ifn->true_branch());
                    true_seq && !true_seq->empty()) {
                    auto* head_if = ast_dyn_cast<IfNode>(true_seq->first());
                    if (head_if && !head_if->false_branch()) {
                        if (auto* outer_cond_ast = ast_dyn_cast<ExprAstNode>(ifn->cond());
                            outer_cond_ast && outer_cond_ast->expr()) {
                            if (auto* inner_cond_ast = ast_dyn_cast<ExprAstNode>(head_if->cond());
                                inner_cond_ast && inner_cond_ast->expr()) {
                                std::optional<bool> implied =
                                    condition_truth_under_outer(outer_cond_ast->expr(), inner_cond_ast->expr());
                                if (implied.has_value()) {
                                    if (implied.value()) {
                                        AstNode* inner_true = head_if->true_branch();
                                        if (inner_true &&
                                            (inner_true->does_end_with_return() ||
                                             inner_true->does_end_with_break() ||
                                             inner_true->does_end_with_continue())) {
                                            ifn->set_true_branch(inner_true);
                                        }
                                    } else {
                                        auto& seq_nodes = true_seq->mutable_nodes();
                                        seq_nodes.erase(seq_nodes.begin());
                                        if (true_seq->empty()) {
                                            ifn->set_true_branch(nullptr);
                                        } else if (true_seq->size() == 1) {
                                            ifn->set_true_branch(true_seq->first());
                                        }
                                    }
                                }
                            }
                        }
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

            for (std::size_t i = 1; i < flattened.size(); ++i) {
                auto* prev_if = ast_dyn_cast<IfNode>(flattened[i - 1]);
                AstNode* trailing = flattened[i];
                if (!prev_if || prev_if->false_branch() != nullptr || !trailing) {
                    continue;
                }

                AstNode* true_branch = prev_if->true_branch();
                if (!true_branch || !true_branch->is_code_node_ending_with_continue()) {
                    continue;
                }

                auto* cond_ast = ast_dyn_cast<ExprAstNode>(prev_if->cond());
                if (!cond_ast || !cond_ast->expr()) {
                    continue;
                }

                const bool trailing_is_break = trailing->is_break_node();
                const bool trailing_is_return = trailing->does_end_with_return();
                if (!trailing_is_break && !trailing_is_return) {
                    continue;
                }

                Expression* negated = negate_condition_expr(arena, cond_ast->expr());
                prev_if->set_cond(arena.create<ExprAstNode>(negated));
                prev_if->set_true_branch(trailing);
                flattened.erase(flattened.begin() + static_cast<long>(i));
                --i;
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
    root = canonicalize_tail_induction_loops(arena, root);
    return clean_node(arena, root);
}

} // namespace aletheia
