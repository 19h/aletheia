#include "car.hpp"
#include "../reachability.hpp"
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

bool ast_contains_node(AstNode* parent, AstNode* target) {
    if (!parent || !target) {
        return false;
    }
    if (parent == target) {
        return true;
    }
    if (auto* seq = ast_dyn_cast<SeqNode>(parent)) {
        for (AstNode* child : seq->nodes()) {
            if (ast_contains_node(child, target)) {
                return true;
            }
        }
        return false;
    }
    if (auto* if_node = ast_dyn_cast<IfNode>(parent)) {
        return ast_contains_node(if_node->true_branch(), target)
            || ast_contains_node(if_node->false_branch(), target);
    }
    if (auto* loop = ast_dyn_cast<LoopNode>(parent)) {
        return ast_contains_node(loop->body(), target);
    }
    if (auto* sw = ast_dyn_cast<SwitchNode>(parent)) {
        for (CaseNode* c : sw->cases()) {
            if (ast_contains_node(c, target)) {
                return true;
            }
        }
        return false;
    }
    if (auto* c = ast_dyn_cast<CaseNode>(parent)) {
        return ast_contains_node(c->body(), target);
    }
    return false;
}

bool ast_contains_original_block(AstNode* node, BasicBlock* target) {
    if (!node || !target) {
        return false;
    }
    if (auto* code = ast_dyn_cast<CodeNode>(node)) {
        return code->block() == target;
    }
    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode* child : seq->nodes()) {
            if (ast_contains_original_block(child, target)) {
                return true;
            }
        }
        return false;
    }
    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        return ast_contains_original_block(if_node->true_branch(), target)
            || ast_contains_original_block(if_node->false_branch(), target);
    }
    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        return ast_contains_original_block(loop->body(), target);
    }
    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* c : sw->cases()) {
            if (ast_contains_original_block(c, target)) {
                return true;
            }
        }
        return false;
    }
    if (auto* c = ast_dyn_cast<CaseNode>(node)) {
        return ast_contains_original_block(c->body(), target);
    }
    return false;
}

int ast_node_transition_match_score(AstNode* node, TransitionBlock* tb) {
    if (!node || !tb) {
        return 0;
    }

    AstNode* tb_node = tb->ast_node();
    if (!tb_node) {
        return 0;
    }

    int score = 0;
    if (tb_node == node) {
        score = std::max(score, 100);
    }
    if (ast_contains_node(tb_node, node) || ast_contains_node(node, tb_node)) {
        score = std::max(score, 80);
    }

    BasicBlock* tb_orig = tb_node->get_original_block();
    BasicBlock* node_orig = node->get_original_block();
    if (tb_orig && node_orig && tb_orig == node_orig) {
        score = std::max(score, 90);
    }
    if (tb_orig && ast_contains_original_block(node, tb_orig)) {
        score = std::max(score, 70);
    }

    return score;
}

std::uint64_t transition_block_order_key(TransitionBlock* tb) {
    if (!tb || !tb->ast_node()) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    auto min_original_block_id = [&](AstNode* node, auto& self) -> std::optional<std::uint64_t> {
        if (!node) {
            return std::nullopt;
        }
        if (BasicBlock* bb = node->get_original_block()) {
            return static_cast<std::uint64_t>(bb->id());
        }

        std::optional<std::uint64_t> best;
        auto take_best = [&](AstNode* child) {
            auto child_id = self(child, self);
            if (child_id.has_value() && (!best.has_value() || child_id.value() < best.value())) {
                best = child_id;
            }
        };

        if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
            for (AstNode* child : seq->nodes()) {
                take_best(child);
            }
        } else if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
            take_best(if_node->true_branch());
            take_best(if_node->false_branch());
        } else if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
            take_best(loop->body());
        } else if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
            for (CaseNode* case_node : sw->cases()) {
                take_best(case_node->body());
            }
        } else if (auto* case_node = ast_dyn_cast<CaseNode>(node)) {
            take_best(case_node->body());
        }

        return best;
    };

    if (auto min_id = min_original_block_id(tb->ast_node(), min_original_block_id); min_id.has_value()) {
        return min_id.value();
    }

    const auto kind_bias = static_cast<std::uint64_t>(tb->ast_node()->ast_kind());
    return std::numeric_limits<std::uint64_t>::max() - kind_bias;
}

const logos::LogicCondition* reaching_condition_for_ast_node(
    const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions,
    AstNode* node
) {
    if (!node) {
        return nullptr;
    }

    const logos::LogicCondition* best = nullptr;
    int best_score = 0;
    std::uint64_t best_key = std::numeric_limits<std::uint64_t>::max();

    for (const auto& [tb, cond] : reaching_conditions) {
        if (!tb) {
            continue;
        }

        const int score = ast_node_transition_match_score(node, tb);
        if (score <= 0) {
            continue;
        }

        const std::uint64_t key = transition_block_order_key(tb);
        if (!best || score > best_score || (score == best_score && key < best_key)) {
            best = &cond;
            best_score = score;
            best_key = key;
        }
    }

    return best;
}

bool condition_can_belong_to_switch(
    const logos::LogicCondition* switch_reaching,
    const logos::LogicCondition* case_reaching
) {
    if (!switch_reaching || !case_reaching) {
        return true;
    }
    return case_reaching->does_imply(*switch_reaching)
        || switch_reaching->does_imply(*case_reaching)
        || case_reaching->is_complementary_to(*switch_reaching)
        || switch_reaching->is_complementary_to(*case_reaching);
}

bool decode_switch_case_operands(Expression* lhs, Expression* rhs,
                                 Expression*& selector, std::string& selector_fp,
                                 std::uint64_t& value) {
    auto* lhs_const = dyn_cast<Constant>(lhs);
    auto* rhs_const = dyn_cast<Constant>(rhs);
    if (lhs_const && !rhs_const) {
        selector = rhs;
        selector_fp = expr_fingerprint(selector);
        value = lhs_const->value();
        return true;
    }
    if (rhs_const && !lhs_const) {
        selector = lhs;
        selector_fp = expr_fingerprint(selector);
        value = rhs_const->value();
        return true;
    }
    return false;
}

bool decode_switch_case_condition(Expression* expr, Expression*& selector, std::string& selector_fp, std::uint64_t& value) {
    auto* cond = dyn_cast<Condition>(expr);
    if (cond && cond->type() == OperationType::eq) {
        return decode_switch_case_operands(cond->lhs(), cond->rhs(), selector, selector_fp, value);
    }

    auto* op = dyn_cast<Operation>(expr);
    if (!op) {
        return false;
    }

    if (op->type() == OperationType::eq && op->operands().size() == 2) {
        return decode_switch_case_operands(op->operands()[0], op->operands()[1], selector, selector_fp, value);
    }

    if (op->type() == OperationType::logical_not && op->operands().size() == 1) {
        Expression* inner = op->operands()[0];

        if (auto* inner_cond = dyn_cast<Condition>(inner);
            inner_cond && inner_cond->type() == OperationType::neq) {
            return decode_switch_case_operands(inner_cond->lhs(), inner_cond->rhs(), selector, selector_fp, value);
        }

        if (auto* inner_op = dyn_cast<Operation>(inner);
            inner_op && inner_op->type() == OperationType::neq && inner_op->operands().size() == 2) {
            return decode_switch_case_operands(inner_op->operands()[0], inner_op->operands()[1], selector, selector_fp, value);
        }
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
    static void run(
        DecompilerArena& arena,
        SeqNode* seq,
        const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions
    ) {
        if (!seq) return;
        std::vector<AstNode*>& nodes = seq->mutable_nodes();

        for (AstNode*& node : nodes) {
            auto* if_node = ast_dyn_cast<IfNode>(node);
            if (!if_node) continue;

            const logos::LogicCondition* chain_reaching =
                reaching_condition_for_ast_node(reaching_conditions, if_node);

            Expression* selector = nullptr;
            std::string selector_fp;
            std::vector<std::pair<std::uint64_t, AstNode*>> cases;
            AstNode* default_branch = nullptr;

            IfNode* cursor = if_node;
            bool valid_chain = true;

            while (cursor) {
                const logos::LogicCondition* cursor_reaching =
                    reaching_condition_for_ast_node(reaching_conditions, cursor);
                if (!condition_can_belong_to_switch(chain_reaching, cursor_reaching)) {
                    valid_chain = false;
                    break;
                }

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
    static void run(
        DecompilerArena& arena,
        SeqNode* seq,
        const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions
    ) {
        if (!seq) return;
        for (AstNode* node : seq->mutable_nodes()) {
            auto* sw = ast_dyn_cast<SwitchNode>(node);
            if (!sw) continue;

            const logos::LogicCondition* switch_reaching =
                reaching_condition_for_ast_node(reaching_conditions, sw);

            Expression* switch_expr = extract_cond_expr(sw->cond());
            if (!switch_expr) continue;
            const std::string switch_fp = expr_fingerprint(switch_expr);

            std::vector<CaseNode*> new_cases;
            for (CaseNode* c : sw->cases()) {
                auto* nested_if = ast_dyn_cast<IfNode>(c->body());
                if (!nested_if) continue;

                const logos::LogicCondition* nested_reaching =
                    reaching_condition_for_ast_node(reaching_conditions, nested_if);
                if (!condition_can_belong_to_switch(switch_reaching, nested_reaching)) {
                    continue;
                }

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
    static void run(
        DecompilerArena& arena,
        SeqNode* seq,
        const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions
    ) {
        if (!seq) return;
        for (AstNode*& node : seq->mutable_nodes()) {
            auto* wrapper_if = ast_dyn_cast<IfNode>(node);
            if (!wrapper_if) continue;

            const logos::LogicCondition* wrapper_reaching =
                reaching_condition_for_ast_node(reaching_conditions, wrapper_if);

            Expression* selector = nullptr;
            std::string selector_fp;
            std::uint64_t case_value = 0;
            if (!decode_switch_case_condition(extract_cond_expr(wrapper_if->cond()), selector, selector_fp, case_value)) {
                continue;
            }

            auto* inner_switch = ast_dyn_cast<SwitchNode>(wrapper_if->false_branch());
            if (!inner_switch) continue;

            const logos::LogicCondition* switch_reaching =
                reaching_condition_for_ast_node(reaching_conditions, inner_switch);
            if (!condition_can_belong_to_switch(switch_reaching, wrapper_reaching)) {
                continue;
            }

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
    static void run(
        DecompilerArena& arena,
        SeqNode* seq,
        const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions
    ) {
        if (!seq) return;
        std::vector<AstNode*>& nodes = seq->mutable_nodes();
        
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto* sw = ast_dyn_cast<SwitchNode>(nodes[i]);
            if (!sw) continue;

            const logos::LogicCondition* switch_reaching =
                reaching_condition_for_ast_node(reaching_conditions, sw);

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

                const logos::LogicCondition* case_reaching =
                    reaching_condition_for_ast_node(reaching_conditions, if_node);
                if (!condition_can_belong_to_switch(switch_reaching, case_reaching)) {
                    break;
                }
                
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
    static void run(
        DecompilerArena& arena,
        SeqNode* seq,
        const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions
    ) {
        if (!seq) return;
        std::vector<AstNode*>& nodes = seq->mutable_nodes();

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto* sw = ast_dyn_cast<SwitchNode>(nodes[i]);
            if (!sw) continue;

            const logos::LogicCondition* switch_reaching =
                reaching_condition_for_ast_node(reaching_conditions, sw);

            Expression* switch_expr = extract_cond_expr(sw->cond());
            if (!switch_expr) continue;
            const std::string switch_fp = expr_fingerprint(switch_expr);

            std::size_t j = i + 1;
            while (j < nodes.size()) {
                auto* if_node = ast_dyn_cast<IfNode>(nodes[j]);
                if (!if_node) break;

                const logos::LogicCondition* case_reaching =
                    reaching_condition_for_ast_node(reaching_conditions, if_node);
                if (!condition_can_belong_to_switch(switch_reaching, case_reaching)) {
                    break;
                }

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

std::string variable_key(const Variable* v) {
    if (!v) {
        return "";
    }
    return v->name() + "#" + std::to_string(v->ssa_version());
}

bool expression_has_side_effects(Expression* expr) {
    if (!expr) {
        return false;
    }

    if (isa<Call>(expr)) {
        return true;
    }

    auto* op = dyn_cast<Operation>(expr);
    if (!op) {
        return false;
    }

    if (op->type() == OperationType::deref || op->type() == OperationType::call) {
        return true;
    }

    for (Expression* child : op->operands()) {
        if (expression_has_side_effects(child)) {
            return true;
        }
    }
    return false;
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

bool ast_writes_any_variable(AstNode* node, const std::unordered_set<std::string>& keys) {
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
            if (ast_writes_any_variable(child, keys)) {
                return true;
            }
        }
        return false;
    }

    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        return ast_writes_any_variable(if_node->true_branch(), keys)
            || ast_writes_any_variable(if_node->false_branch(), keys);
    }

    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        return ast_writes_any_variable(loop->body(), keys);
    }

    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* case_node : sw->cases()) {
            if (ast_writes_any_variable(case_node->body(), keys)) {
                return true;
            }
        }
        return false;
    }

    if (auto* case_node = ast_dyn_cast<CaseNode>(node)) {
        return ast_writes_any_variable(case_node->body(), keys);
    }

    return false;
}

AstNode* strip_terminal_continue(AstNode* node) {
    if (!node) {
        return nullptr;
    }

    if (auto* code = ast_dyn_cast<CodeNode>(node)) {
        if (code->does_end_with_continue()) {
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

    AstNode* stripped_tail = strip_terminal_continue(seq->last());
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

AstNode* strip_terminal_break(AstNode* node) {
    if (!node) {
        return nullptr;
    }

    if (auto* code = ast_dyn_cast<CodeNode>(node)) {
        if (code->does_end_with_break()) {
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

    AstNode* stripped_tail = strip_terminal_break(seq->last());
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

void rewrite_tail_driven_endless_loops(DecompilerArena& arena, SeqNode* seq) {
    if (!seq) {
        return;
    }

    auto& nodes = seq->mutable_nodes();
    for (AstNode*& node : nodes) {
        auto* while_node = ast_dyn_cast<WhileLoopNode>(node);
        if (!while_node || !while_node->is_endless() || !while_node->body()) {
            continue;
        }

        auto* body_seq = ast_dyn_cast<SeqNode>(while_node->body());
        if (!body_seq || body_seq->size() < 2) {
            continue;
        }

        auto& body_nodes = body_seq->mutable_nodes();
        AstNode* tail_break = body_nodes.back();
        auto* tail_if = ast_dyn_cast<IfNode>(body_nodes[body_nodes.size() - 2]);
        if (!tail_if || tail_if->false_branch() != nullptr) {
            continue;
        }

        Expression* cond_expr = tail_if->condition_expr();
        if (!cond_expr || expression_has_side_effects(cond_expr)) {
            continue;
        }

        if (!tail_if->true_branch() || !tail_if->true_branch()->does_end_with_continue()) {
            continue;
        }
        if (!tail_break->does_end_with_break()) {
            continue;
        }

        std::unordered_set<std::string> cond_keys;
        collect_condition_variable_keys(cond_expr, cond_keys);
        const bool body_updates_cond = ast_writes_any_variable(body_seq, cond_keys);
        if (cond_keys.empty() || !body_updates_cond) {
            continue;
        }

        AstNode* true_body = strip_terminal_continue(tail_if->true_branch());
        if (!true_body) {
            continue;
        }
        AstNode* false_body = strip_terminal_break(tail_break);

        AstNode* replacement_tail = true_body;
        if (false_body) {
            replacement_tail = arena.create<IfNode>(tail_if->cond(), true_body, false_body);
        }

        body_nodes[body_nodes.size() - 2] = replacement_tail;
        body_nodes.pop_back();

        AstNode* do_body = body_seq;
        if (body_seq->size() == 1) {
            do_body = body_seq->first();
        }

        node = arena.create<DoWhileLoopNode>(do_body, cond_expr);
    }
}

AstNode* rewrite_guarded_do_while(DecompilerArena& arena, AstNode* node) {
    if (!node) return nullptr;

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode*& child : seq->mutable_nodes()) {
            child = rewrite_guarded_do_while(arena, child);
        }
        rewrite_tail_driven_endless_loops(arena, seq);
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

AstNode* refine_condition_aware_recursive(
    DecompilerArena& arena,
    AstNode* node,
    const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions
) {
    if (!node) {
        return nullptr;
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode*& child : seq->mutable_nodes()) {
            child = refine_condition_aware_recursive(arena, child, reaching_conditions);
        }

        InitialSwitchNodeConstructor::run(arena, seq, reaching_conditions);
        MissingCaseFinderCondition::run(arena, seq, reaching_conditions);
        SwitchExtractor::run(arena, seq, reaching_conditions);
        MissingCaseFinderSequence::run(arena, seq, reaching_conditions);
        MissingCaseFinderIntersectingConstants::run(arena, seq, reaching_conditions);

        for (AstNode* child : seq->mutable_nodes()) {
            if (auto* sw = ast_dyn_cast<SwitchNode>(child)) {
                sw->mutable_cases() = CaseDependencyGraph::order_cases(sw->cases());
            }
        }

        AstNode* rewritten = rewrite_guarded_do_while(arena, seq);
        if (rewritten != node) {
            return refine_condition_aware_recursive(arena, rewritten, reaching_conditions);
        }
        return rewritten;
    }

    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        if_node->set_cond(refine_condition_aware_recursive(arena, if_node->cond(), reaching_conditions));
        if_node->set_true_branch(refine_condition_aware_recursive(arena, if_node->true_branch(), reaching_conditions));
        if_node->set_false_branch(refine_condition_aware_recursive(arena, if_node->false_branch(), reaching_conditions));
        return if_node;
    }

    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        loop->set_body(refine_condition_aware_recursive(arena, loop->body(), reaching_conditions));
        return loop;
    }

    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* c : sw->mutable_cases()) {
            c->set_body(refine_condition_aware_recursive(arena, c->body(), reaching_conditions));
        }
        sw->mutable_cases() = CaseDependencyGraph::order_cases(sw->cases());
        return sw;
    }

    if (auto* c = ast_dyn_cast<CaseNode>(node)) {
        c->set_body(refine_condition_aware_recursive(arena, c->body(), reaching_conditions));
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
    return refine_condition_aware_recursive(arena, root, reaching_conditions);
}

} // namespace aletheia
