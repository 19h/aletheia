#include "car.hpp"
#include "../reachability.hpp"
#include <vector>
#include <string>

namespace aletheia {

namespace {

Expression* extract_cond_expr(AstNode* cond_node) {
    if (auto* expr_ast = ast_dyn_cast<ExprAstNode>(cond_node)) {
        return expr_ast->expr();
    }
    return nullptr;
}

std::string expr_fingerprint(Expression* expr) {
    if (!expr) return "<null>";
    if (auto* c = dyn_cast<Constant>(expr)) {
        return "C:" + std::to_string(c->value()) + ":" + std::to_string(c->size_bytes);
    }
    if (auto* v = dyn_cast<Variable>(expr)) {
        return "V:" + v->name() + ":" + std::to_string(v->ssa_version());
    }
    if (auto* op = dyn_cast<Operation>(expr)) {
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
    auto* cond = dyn_cast<Condition>(expr);
    if (!cond || cond->type() != OperationType::eq) return false;

    auto* lhs_const = dyn_cast<Constant>(cond->lhs());
    auto* rhs_const = dyn_cast<Constant>(cond->rhs());
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
            auto* if_node = ast_dyn_cast<IfNode>(node);
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
                if (auto* nested = ast_dyn_cast<IfNode>(false_branch)) {
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
            auto* sw = ast_dyn_cast<SwitchNode>(node);
            if (!sw) continue;

            Expression* switch_expr = extract_cond_expr(sw->cond());
            if (!switch_expr) continue;
            const std::string switch_fp = expr_fingerprint(switch_expr);

            std::vector<CaseNode*> new_cases;
            for (CaseNode* c : sw->cases()) {
                auto* nested_if = ast_dyn_cast<IfNode>(c->body());
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
            auto* wrapper_if = ast_dyn_cast<IfNode>(node);
            if (!wrapper_if) continue;

            Expression* selector = nullptr;
            std::string selector_fp;
            std::uint64_t case_value = 0;
            if (!decode_switch_case_condition(extract_cond_expr(wrapper_if->cond()), selector, selector_fp, case_value)) {
                continue;
            }

            auto* inner_switch = ast_dyn_cast<SwitchNode>(wrapper_if->false_branch());
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


class MissingCaseFinderIntersectingConstants {
public:
    static void run(DecompilerArena& arena, SeqNode* seq) {
        if (!seq) return;
        std::vector<AstNode*>& nodes = seq->mutable_nodes();
        
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto* sw = ast_dyn_cast<SwitchNode>(nodes[i]);
            if (!sw) continue;

            Expression* switch_expr = extract_cond_expr(sw->cond());
            if (!switch_expr) continue;
            const std::string switch_fp = expr_fingerprint(switch_expr);

            // In C++, we often see cascading if-else structures or independent
            // sequence nodes acting as missing cases that check bitwise intersections
            // or intervals. For now, we perform a simplified interval / multi-case 
            // merge targeting adjacent if statements.
            
            std::size_t j = i + 1;
            while (j < nodes.size()) {
                auto* if_node = ast_dyn_cast<IfNode>(nodes[j]);
                if (!if_node) break;
                
                // If it is checking the same switch_expr via multiple OR conditions
                // we can recover it. We need to check if the condition checks the same variable.
                Expression* cond_expr = extract_cond_expr(if_node->cond());
                if (!cond_expr) break;
                
                std::vector<std::uint64_t> values;
                if (extract_multiple_cases(cond_expr, switch_fp, values)) {
                    if (!values.empty()) {
                        for (std::uint64_t v : values) {
                            CaseNode* c = arena.create<CaseNode>(v, if_node->true_branch());
                            sw->add_case(c);
                        }
                        
                        nodes.erase(nodes.begin() + j);
                        // Do not increment j, we want to check the new nodes[j]
                        continue;
                    }
                }
                break;
            }
        }
    }

private:
    static bool extract_multiple_cases(Expression* expr, const std::string& switch_fp, std::vector<std::uint64_t>& values) {
        if (!expr) return false;
        if (auto* op = dyn_cast<Operation>(expr)) {
            if (op->type() == OperationType::logical_or) {
                bool ok = true;
                for (Expression* child : op->operands()) {
                    if (!extract_multiple_cases(child, switch_fp, values)) {
                        ok = false;
                        break;
                    }
                }
                return ok;
            } else if (op->type() == OperationType::eq) {
                if (op->operands().size() == 2) {
                    Expression* lhs = op->operands()[0];
                    Expression* rhs = op->operands()[1];
                    if (expr_fingerprint(lhs) == switch_fp) {
                        if (auto* c = dyn_cast<Constant>(rhs)) {
                            values.push_back(c->value());
                            return true;
                        }
                    } else if (expr_fingerprint(rhs) == switch_fp) {
                        if (auto* c = dyn_cast<Constant>(lhs)) {
                            values.push_back(c->value());
                            return true;
                        }
                    }
                }
            } else if (op->type() == OperationType::bit_and && op->operands().size() == 2) {
                // Heuristic for bitwise boundary check recovery (common compiler optimization for switch)
                // e.g., (x & mask) == val
                // Since full recovery requires VSA/RangeSimplifier integration which is currently
                // isolated from DAG, we conservatively only match explicit multi-case ORs here.
            }
        }
        return false;
    }
};

class MissingCaseFinderSequence {
public:
    static void run(DecompilerArena& arena, SeqNode* seq) {
        if (!seq) return;
        std::vector<AstNode*>& nodes = seq->mutable_nodes();

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto* sw = ast_dyn_cast<SwitchNode>(nodes[i]);
            if (!sw) continue;

            Expression* switch_expr = extract_cond_expr(sw->cond());
            if (!switch_expr) continue;
            const std::string switch_fp = expr_fingerprint(switch_expr);

            std::size_t j = i + 1;
            while (j < nodes.size()) {
                auto* if_node = ast_dyn_cast<IfNode>(nodes[j]);
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

bool same_variable(const Variable* lhs, const Variable* rhs) {
    if (!lhs || !rhs) return false;
    return lhs->name() == rhs->name() && lhs->ssa_version() == rhs->ssa_version();
}

Variable* extract_condition_variable(Expression* expr) {
    auto* cond = dyn_cast<Condition>(expr);
    if (!cond) return nullptr;

    if (auto* lhs_var = dyn_cast<Variable>(cond->lhs())) {
        return lhs_var;
    }
    if (auto* rhs_var = dyn_cast<Variable>(cond->rhs())) {
        return rhs_var;
    }
    return nullptr;
}

Assignment* extract_initializer_assignment(CodeNode* node) {
    if (!node || !node->block()) return nullptr;
    const auto& insts = node->block()->instructions();
    if (insts.empty()) return nullptr;
    return dyn_cast<Assignment>(insts.back());
}

Assignment* extract_trailing_assignment(AstNode* body, CodeNode** owner_code) {
    if (!body) return nullptr;

    if (auto* code = ast_dyn_cast<CodeNode>(body)) {
        if (!code->block() || code->block()->instructions().empty()) return nullptr;
        Instruction* tail = code->block()->instructions().back();
        auto* assign = dyn_cast<Assignment>(tail);
        if (assign && owner_code) *owner_code = code;
        return assign;
    }

    auto* seq = ast_dyn_cast<SeqNode>(body);
    if (!seq || seq->nodes().empty()) return nullptr;
    auto* tail_code = ast_dyn_cast<CodeNode>(seq->last());
    if (!tail_code || !tail_code->block() || tail_code->block()->instructions().empty()) return nullptr;
    Instruction* tail = tail_code->block()->instructions().back();
    auto* assign = dyn_cast<Assignment>(tail);
    if (assign && owner_code) *owner_code = tail_code;
    return assign;
}

bool is_induction_update(Assignment* assign, Variable* variable) {
    if (!assign || !variable) return false;
    auto* dst = dyn_cast<Variable>(assign->destination());
    if (!same_variable(dst, variable)) return false;

    auto* op = dyn_cast<Operation>(assign->value());
    if (!op || op->operands().size() != 2) return false;
    if (op->type() != OperationType::add && op->type() != OperationType::sub) return false;

    auto* lhs_var = dyn_cast<Variable>(op->operands()[0]);
    auto* rhs_var = dyn_cast<Variable>(op->operands()[1]);
    return same_variable(lhs_var, variable) || same_variable(rhs_var, variable);
}

void rewrite_while_to_for_in_sequence(DecompilerArena& arena, SeqNode* seq) {
    if (!seq) return;
    auto& nodes = seq->mutable_nodes();
    if (nodes.size() < 2) return;

    for (std::size_t i = 1; i < nodes.size(); ++i) {
        auto* while_node = ast_dyn_cast<WhileLoopNode>(nodes[i]);
        if (!while_node || while_node->condition() == nullptr || while_node->body() == nullptr) {
            continue;
        }

        auto* init_code = ast_dyn_cast<CodeNode>(nodes[i - 1]);
        auto* init_assign = extract_initializer_assignment(init_code);
        if (!init_assign) continue;

        auto* init_var = dyn_cast<Variable>(init_assign->destination());
        auto* cond_var = extract_condition_variable(while_node->condition());
        if (!same_variable(init_var, cond_var)) continue;

        CodeNode* mod_owner = nullptr;
        auto* mod_assign = extract_trailing_assignment(while_node->body(), &mod_owner);
        if (!mod_assign) continue;

        auto* mod_var = dyn_cast<Variable>(mod_assign->destination());
        if (!same_variable(mod_var, init_var) || !is_induction_update(mod_assign, init_var)) {
            continue;
        }

        if (mod_owner) {
            mod_owner->remove_last_instruction();
        }
        init_code->remove_last_instruction();

        auto* for_node = arena.create<ForLoopNode>(
            while_node->body(),
            while_node->condition(),
            init_assign,
            mod_assign);

        nodes[i] = for_node;
        if (init_code->block()->instructions().empty()) {
            nodes.erase(nodes.begin() + static_cast<long>(i - 1));
            if (i > 1) {
                --i;
            }
        }
    }
}

AstNode* rewrite_guarded_do_while(DecompilerArena& arena, AstNode* node) {
    if (!node) return nullptr;

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode*& child : seq->mutable_nodes()) {
            child = rewrite_guarded_do_while(arena, child);
        }
        rewrite_while_to_for_in_sequence(arena, seq);
        return seq;
    }

    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        if (if_node->true_branch()) {
            if_node->set_true_branch(rewrite_guarded_do_while(arena, if_node->true_branch()));
        }
        if (if_node->false_branch()) {
            if_node->set_false_branch(rewrite_guarded_do_while(arena, if_node->false_branch()));
        }

        // Guarded do-while pattern:
        // if (cond) { do { body } while (cond); }  -->  while (cond) { body }
        if (if_node->false_branch() == nullptr) {
            auto* dowhile = ast_dyn_cast<DoWhileLoopNode>(if_node->true_branch());
            Expression* guard_cond = if_node->condition_expr();
            if (dowhile != nullptr && guard_cond != nullptr && dowhile->condition() != nullptr) {
                if (expr_fingerprint(guard_cond) == expr_fingerprint(dowhile->condition())) {
                    return arena.create<WhileLoopNode>(dowhile->body(), dowhile->condition());
                }
            }
        }

        return if_node;
    }

    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        if (loop->body()) {
            loop->set_body(rewrite_guarded_do_while(arena, loop->body()));
        }
        return loop;
    }

    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* c : sw->mutable_cases()) {
            c->set_body(rewrite_guarded_do_while(arena, c->body()));
        }
        return sw;
    }

    if (auto* c = ast_dyn_cast<CaseNode>(node)) {
        c->set_body(rewrite_guarded_do_while(arena, c->body()));
        return c;
    }

    return node;
}

AstNode* refine_condition_aware_recursive(DecompilerArena& arena, AstNode* node) {
    if (!node) {
        return nullptr;
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode*& child : seq->mutable_nodes()) {
            child = refine_condition_aware_recursive(arena, child);
        }

        InitialSwitchNodeConstructor::run(arena, seq);
        MissingCaseFinderCondition::run(arena, seq);
        SwitchExtractor::run(arena, seq);
        MissingCaseFinderSequence::run(arena, seq);
        MissingCaseFinderIntersectingConstants::run(arena, seq);

        for (AstNode* child : seq->mutable_nodes()) {
            if (auto* sw = ast_dyn_cast<SwitchNode>(child)) {
                sw->mutable_cases() = CaseDependencyGraph::order_cases(sw->cases());
            }
        }

        AstNode* rewritten = rewrite_guarded_do_while(arena, seq);
        if (rewritten != node) {
            return refine_condition_aware_recursive(arena, rewritten);
        }
        return rewritten;
    }

    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        if_node->set_cond(refine_condition_aware_recursive(arena, if_node->cond()));
        if_node->set_true_branch(refine_condition_aware_recursive(arena, if_node->true_branch()));
        if_node->set_false_branch(refine_condition_aware_recursive(arena, if_node->false_branch()));
        return if_node;
    }

    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        loop->set_body(refine_condition_aware_recursive(arena, loop->body()));
        return loop;
    }

    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* c : sw->mutable_cases()) {
            c->set_body(refine_condition_aware_recursive(arena, c->body()));
        }
        sw->mutable_cases() = CaseDependencyGraph::order_cases(sw->cases());
        return sw;
    }

    if (auto* c = ast_dyn_cast<CaseNode>(node)) {
        c->set_body(refine_condition_aware_recursive(arena, c->body()));
        return c;
    }

    return node;
}

} // namespace

AstNode* ConditionAwareRefinement::refine(
    DecompilerArena& arena,
    z3::context& ctx,
    AstNode* root,
    const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions
) {
    (void)ctx;
    (void)reaching_conditions;
    return refine_condition_aware_recursive(arena, root);
}

} // namespace aletheia
