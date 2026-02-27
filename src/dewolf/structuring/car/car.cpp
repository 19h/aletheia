#include "car.hpp"
#include "../reachability.hpp"
#include <vector>
#include <string>

namespace dewolf {

namespace {

Expression* extract_cond_expr(AstNode* cond_node) {
    if (auto* expr_ast = dynamic_cast<ExprAstNode*>(cond_node)) {
        return expr_ast->expr();
    }
    return nullptr;
}

std::string expr_fingerprint(Expression* expr) {
    if (!expr) return "<null>";
    if (auto* c = dynamic_cast<Constant*>(expr)) {
        return "C:" + std::to_string(c->value()) + ":" + std::to_string(c->size_bytes);
    }
    if (auto* v = dynamic_cast<Variable*>(expr)) {
        return "V:" + v->name() + ":" + std::to_string(v->ssa_version());
    }
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        std::string out = "O:" + std::to_string(static_cast<int>(op->type())) + "(";
        bool first = true;
        for (auto* child : op->operands()) {
            if (!first) out += ",";
            first = false;
            out += expr_fingerprint(child);
        }
        out += ")";
        return out;
    }
    return "E:unknown";
}

bool decode_switch_case_condition(Expression* expr, Expression*& selector, std::string& selector_fp, std::uint64_t& value) {
    auto* cond = dynamic_cast<Condition*>(expr);
    if (!cond || cond->type() != OperationType::eq) return false;

    auto* lhs_const = dynamic_cast<Constant*>(cond->lhs());
    auto* rhs_const = dynamic_cast<Constant*>(cond->rhs());
    if (lhs_const && !rhs_const) {
        selector = cond->rhs();
        selector_fp = expr_fingerprint(selector);
        value = lhs_const->value();
        return true;
    }
    if (rhs_const && !lhs_const) {
        selector = cond->lhs();
        selector_fp = expr_fingerprint(selector);
        value = rhs_const->value();
        return true;
    }
    return false;
}

bool switch_has_case_value(SwitchNode* sw, std::uint64_t value) {
    for (CaseNode* c : sw->cases()) {
        if (!c->is_default() && c->value() == value) return true;
    }
    return false;
}

bool switch_has_default_case(SwitchNode* sw) {
    for (CaseNode* c : sw->cases()) {
        if (c->is_default()) return true;
    }
    return false;
}

class InitialSwitchNodeConstructor {
public:
    static void run(DecompilerArena& arena, SeqNode* seq) {
        if (!seq) return;
        std::vector<AstNode*>& nodes = seq->mutable_nodes();

        for (AstNode*& node : nodes) {
            auto* if_node = dynamic_cast<IfNode*>(node);
            if (!if_node) continue;

            Expression* selector = nullptr;
            std::string selector_fp;
            std::vector<std::pair<std::uint64_t, AstNode*>> cases;
            AstNode* default_branch = nullptr;

            IfNode* cursor = if_node;
            bool valid_chain = true;

            while (cursor) {
                Expression* cond_expr = extract_cond_expr(cursor->cond());
                Expression* curr_selector = nullptr;
                std::string curr_selector_fp;
                std::uint64_t case_value = 0;

                if (!decode_switch_case_condition(cond_expr, curr_selector, curr_selector_fp, case_value)) {
                    valid_chain = false;
                    break;
                }

                if (!selector) {
                    selector = curr_selector;
                    selector_fp = curr_selector_fp;
                } else if (selector_fp != curr_selector_fp) {
                    valid_chain = false;
                    break;
                }

                cases.push_back({case_value, cursor->true_branch()});

                AstNode* false_branch = cursor->false_branch();
                if (auto* nested = dynamic_cast<IfNode*>(false_branch)) {
                    cursor = nested;
                } else {
                    default_branch = false_branch;
                    cursor = nullptr;
                }
            }

            if (!valid_chain || !selector || cases.size() < 2) continue;

            auto* sw = arena.create<SwitchNode>(arena.create<ExprAstNode>(selector));
            for (auto& [value, body] : cases) {
                sw->add_case(arena.create<CaseNode>(value, body));
            }
            if (default_branch) {
                sw->add_case(arena.create<CaseNode>(0, default_branch, true));
            }
            node = sw;
        }
    }
};

class MissingCaseFinderCondition {
public:
    static void run(DecompilerArena& arena, SeqNode* seq) {
        if (!seq) return;
        for (AstNode* node : seq->mutable_nodes()) {
            auto* sw = dynamic_cast<SwitchNode*>(node);
            if (!sw) continue;

            Expression* switch_expr = extract_cond_expr(sw->cond());
            if (!switch_expr) continue;
            const std::string switch_fp = expr_fingerprint(switch_expr);

            std::vector<CaseNode*> new_cases;
            for (CaseNode* c : sw->cases()) {
                auto* nested_if = dynamic_cast<IfNode*>(c->body());
                if (!nested_if) continue;

                Expression* nested_selector = nullptr;
                std::string nested_fp;
                std::uint64_t nested_value = 0;
                if (!decode_switch_case_condition(extract_cond_expr(nested_if->cond()), nested_selector, nested_fp, nested_value)) {
                    continue;
                }

                if (nested_fp != switch_fp || switch_has_case_value(sw, nested_value)) {
                    continue;
                }

                new_cases.push_back(arena.create<CaseNode>(nested_value, nested_if->true_branch()));
                if (nested_if->false_branch()) {
                    c->set_body(nested_if->false_branch());
                } else {
                    c->set_body(arena.create<SeqNode>());
                }
            }

            for (CaseNode* extracted : new_cases) {
                sw->add_case(extracted);
            }
        }
    }
};

class SwitchExtractor {
public:
    static void run(DecompilerArena& arena, SeqNode* seq) {
        if (!seq) return;
        for (AstNode*& node : seq->mutable_nodes()) {
            auto* wrapper_if = dynamic_cast<IfNode*>(node);
            if (!wrapper_if) continue;

            Expression* selector = nullptr;
            std::string selector_fp;
            std::uint64_t case_value = 0;
            if (!decode_switch_case_condition(extract_cond_expr(wrapper_if->cond()), selector, selector_fp, case_value)) {
                continue;
            }

            auto* inner_switch = dynamic_cast<SwitchNode*>(wrapper_if->false_branch());
            if (!inner_switch) continue;

            Expression* inner_selector = extract_cond_expr(inner_switch->cond());
            if (!inner_selector || expr_fingerprint(inner_selector) != selector_fp) continue;

            auto* merged = arena.create<SwitchNode>(arena.create<ExprAstNode>(selector));
            merged->add_case(arena.create<CaseNode>(case_value, wrapper_if->true_branch()));
            for (CaseNode* c : inner_switch->cases()) {
                if (c->is_default()) {
                    merged->add_case(arena.create<CaseNode>(0, c->body(), true, c->break_case()));
                } else if (!switch_has_case_value(merged, c->value())) {
                    merged->add_case(arena.create<CaseNode>(c->value(), c->body(), false, c->break_case()));
                }
            }
            node = merged;
        }
    }
};

class MissingCaseFinderSequence {
public:
    static void run(DecompilerArena& arena, SeqNode* seq) {
        if (!seq) return;
        std::vector<AstNode*>& nodes = seq->mutable_nodes();

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto* sw = dynamic_cast<SwitchNode*>(nodes[i]);
            if (!sw) continue;

            Expression* switch_expr = extract_cond_expr(sw->cond());
            if (!switch_expr) continue;
            const std::string switch_fp = expr_fingerprint(switch_expr);

            std::size_t j = i + 1;
            while (j < nodes.size()) {
                auto* if_node = dynamic_cast<IfNode*>(nodes[j]);
                if (!if_node) break;

                Expression* selector = nullptr;
                std::string selector_fp;
                std::uint64_t value = 0;
                if (!decode_switch_case_condition(extract_cond_expr(if_node->cond()), selector, selector_fp, value)) {
                    break;
                }
                if (selector_fp != switch_fp) break;

                if (!switch_has_case_value(sw, value)) {
                    sw->add_case(arena.create<CaseNode>(value, if_node->true_branch()));
                }

                if (if_node->false_branch() && !switch_has_default_case(sw)) {
                    sw->add_case(arena.create<CaseNode>(0, if_node->false_branch(), true));
                }

                nodes.erase(nodes.begin() + static_cast<long>(j));
            }
        }
    }
};

} // namespace

AstNode* ConditionAwareRefinement::refine(
    DecompilerArena& arena,
    z3::context& ctx,
    AstNode* root,
    const std::unordered_map<TransitionBlock*, dewolf_logic::LogicCondition>& reaching_conditions
) {
    if (auto* seq = dynamic_cast<SeqNode*>(root)) {
        (void)ctx;
        (void)reaching_conditions;

        InitialSwitchNodeConstructor::run(arena, seq);
        MissingCaseFinderCondition::run(arena, seq);
        SwitchExtractor::run(arena, seq);
        MissingCaseFinderSequence::run(arena, seq);

        for (AstNode* node : seq->mutable_nodes()) {
            if (auto* sw = dynamic_cast<SwitchNode*>(node)) {
                sw->mutable_cases() = CaseDependencyGraph::order_cases(sw->cases());
            }
        }

        return seq;
    }

    return root;
}

} // namespace dewolf
