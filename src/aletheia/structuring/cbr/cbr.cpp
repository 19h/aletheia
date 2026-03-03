#include "cbr.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <vector>

namespace {

using namespace aletheia;

bool ast_contains(AstNode* parent, AstNode* target) {
    if (parent == target) return true;
    if (!parent) return false;
    if (auto* seq = ast_dyn_cast<SeqNode>(parent)) {
        for (auto* c : seq->nodes()) if (ast_contains(c, target)) return true;
    } else if (auto* ifn = ast_dyn_cast<IfNode>(parent)) {
        if (ast_contains(ifn->true_branch(), target)) return true;
        if (ast_contains(ifn->false_branch(), target)) return true;
    } else if (auto* loop = ast_dyn_cast<LoopNode>(parent)) {
        if (ast_contains(loop->body(), target)) return true;
    } else if (auto* sw = ast_dyn_cast<SwitchNode>(parent)) {
        for (auto* c : sw->cases()) if (ast_contains(c, target)) return true;
    } else if (auto* c = ast_dyn_cast<CaseNode>(parent)) {
        if (ast_contains(c->body(), target)) return true;
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
    if (auto* ifn = ast_dyn_cast<IfNode>(node)) {
        return ast_contains_original_block(ifn->true_branch(), target)
            || ast_contains_original_block(ifn->false_branch(), target);
    }
    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        return ast_contains_original_block(loop->body(), target);
    }
    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* case_node : sw->cases()) {
            if (ast_contains_original_block(case_node, target)) {
                return true;
            }
        }
        return false;
    }
    if (auto* case_node = ast_dyn_cast<CaseNode>(node)) {
        return ast_contains_original_block(case_node->body(), target);
    }
    return false;
}

bool ast_node_matches_transition_block(AstNode* node, TransitionBlock* tb) {
    if (!node || !tb) {
        return false;
    }

    AstNode* tb_node = tb->ast_node();
    if (!tb_node) {
        return false;
    }

    if (tb_node == node || ast_contains(tb_node, node) || ast_contains(node, tb_node)) {
        return true;
    }

    BasicBlock* tb_orig = tb_node->get_original_block();
    BasicBlock* node_orig = node->get_original_block();
    if (tb_orig && node_orig && tb_orig == node_orig) {
        return true;
    }
    if (tb_orig && ast_contains_original_block(node, tb_orig)) {
        return true;
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

    if (ast_contains(tb_node, node) || ast_contains(node, tb_node)) {
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

bool cbr_debug_enabled() {
    const char* value = std::getenv("ALETHEIA_CBR_DEBUG");
    if (!value) return false;
    std::string v{value};
    return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
}

Expression* if_condition_expr(IfNode* node) {
    if (!node) return nullptr;
    if (auto* expr_ast = ast_dyn_cast<ExprAstNode>(node->cond())) {
        return expr_ast->expr();
    }
    return nullptr;
}

std::optional<bool> classify_node_by_ast_condition(
    z3::context& ctx,
    AstNode* node,
    const logos::LogicCondition& extracted_cond
) {
    auto* if_node = ast_dyn_cast<IfNode>(node);
    if (!if_node) {
        return std::nullopt;
    }

    Expression* cond_expr = if_condition_expr(if_node);
    if (!cond_expr) {
        return std::nullopt;
    }

    logos::Z3Converter conv(ctx);
    logos::LogicCondition node_cond = conv.convert_to_condition(cond_expr);
    if (node_cond.does_imply(extracted_cond) || extracted_cond.does_imply(node_cond)) {
        return true;
    }
    if (node_cond.is_complementary_to(extracted_cond)) {
        return false;
    }

    return std::nullopt;
}

TransitionBlock* transition_block_for_ast_node(
    const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions,
    AstNode* node
) {
    if (!node) {
        return nullptr;
    }
    TransitionBlock* best = nullptr;
    int best_score = 0;
    std::uint64_t best_key = std::numeric_limits<std::uint64_t>::max();

    for (const auto& [tb, cond] : reaching_conditions) {
        (void)cond;
        if (!tb) {
            continue;
        }

        const int score = ast_node_transition_match_score(node, tb);
        if (score <= 0) {
            continue;
        }

        const std::uint64_t key = transition_block_order_key(tb);
        if (!best || score > best_score || (score == best_score && key < best_key)) {
            best = tb;
            best_score = score;
            best_key = key;
        }
    }

    return best;
}

bool ast_node_is_terminal(AstNode* node) {
    if (!node) {
        return false;
    }
    return node->does_end_with_return() || node->does_end_with_break() || node->does_end_with_continue();
}

bool ast_node_appears_later(const std::vector<AstNode*>& nodes, std::size_t start, AstNode* needle) {
    if (!needle) {
        return false;
    }
    for (std::size_t i = start; i < nodes.size(); ++i) {
        AstNode* candidate = nodes[i];
        if (!candidate) {
            continue;
        }
        if (candidate == needle || ast_contains(candidate, needle) || ast_contains(needle, candidate)) {
            return true;
        }
    }
    return false;
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

void flatten_conjunction(Expression* expr, std::vector<Expression*>& out) {
    if (!expr) return;
    auto* op = dyn_cast<Operation>(expr);
    if (op && op->type() == OperationType::logical_and && op->operands().size() == 2) {
        flatten_conjunction(op->operands()[0], out);
        flatten_conjunction(op->operands()[1], out);
        return;
    }
    out.push_back(expr);
}

void flatten_disjunction(Expression* expr, std::vector<Expression*>& out) {
    if (!expr) return;
    auto* op = dyn_cast<Operation>(expr);
    if (op && op->type() == OperationType::logical_or && op->operands().size() == 2) {
        flatten_disjunction(op->operands()[0], out);
        flatten_disjunction(op->operands()[1], out);
        return;
    }
    out.push_back(expr);
}

Expression* rebuild_conjunction(DecompilerArena& arena, const std::vector<Expression*>& terms, Expression* original) {
    if (terms.empty()) return nullptr;
    if (terms.size() == 1) return terms[0];

    Expression* acc = terms[0];
    for (std::size_t i = 1; i < terms.size(); ++i) {
        auto* op = arena.create<Operation>(OperationType::logical_and, std::vector<Expression*>{acc, terms[i]}, 1);
        if (original) {
            op->set_ir_type(original->ir_type());
        }
        acc = op;
    }
    return acc;
}

Expression* remove_clause_from_condition(DecompilerArena& arena, Expression* cond_expr, const std::string& clause_id) {
    std::vector<Expression*> clauses;
    flatten_conjunction(cond_expr, clauses);
    if (clauses.empty()) return cond_expr;

    std::vector<Expression*> remaining;
    remaining.reserve(clauses.size());

    bool removed = false;
    for (Expression* clause : clauses) {
        if (!removed && expr_fingerprint(clause) == clause_id) {
            removed = true;
            continue;
        }
        remaining.push_back(clause);
    }

    if (!removed) return cond_expr;
    return rebuild_conjunction(arena, remaining, cond_expr);
}

class ConditionCandidates {
public:
    explicit ConditionCandidates(const std::vector<IfNode*>& candidates) {
        for (IfNode* candidate : candidates) {
            register_candidate(candidate);
        }
    }

    bool shared_clause(IfNode* a, IfNode* b, std::string* shared_clause_id) const {
        auto ia = formula_to_clauses_.find(a);
        auto ib = formula_to_clauses_.find(b);
        if (ia == formula_to_clauses_.end() || ib == formula_to_clauses_.end()) return false;

        for (const std::string& clause_a : ia->second) {
            for (const std::string& clause_b : ib->second) {
                if (clause_a == clause_b) {
                    if (shared_clause_id) {
                        *shared_clause_id = clause_a;
                    }
                    return true;
                }
            }
        }
        return false;
    }

    Expression* clause_expression(const std::string& clause_id) const {
        auto it = clause_to_expression_.find(clause_id);
        if (it == clause_to_expression_.end()) return nullptr;
        return it->second;
    }

private:
    void register_candidate(IfNode* candidate) {
        Expression* cond = if_condition_expr(candidate);
        if (!cond) return;

        std::vector<Expression*> clauses;
        flatten_conjunction(cond, clauses);

        auto& clause_ids = formula_to_clauses_[candidate];
        for (Expression* clause_expr : clauses) {
            const std::string clause_id = expr_fingerprint(clause_expr);
            clause_ids.push_back(clause_id);
            if (!clause_to_expression_.contains(clause_id)) {
                clause_to_expression_[clause_id] = clause_expr;
            }

            std::vector<Expression*> symbols;
            flatten_disjunction(clause_expr, symbols);
            auto& symbol_set = clause_to_symbols_[clause_id];
            for (Expression* symbol_expr : symbols) {
                symbol_set.insert(expr_fingerprint(symbol_expr));
            }
        }
    }

    std::unordered_map<IfNode*, std::vector<std::string>> formula_to_clauses_;
    std::unordered_map<std::string, std::unordered_set<std::string>> clause_to_symbols_;
    std::unordered_map<std::string, Expression*> clause_to_expression_;
};

void apply_complementary_condition_pairing(
    DecompilerArena& arena,
    z3::context& ctx,
    std::vector<AstNode*>& nodes) {
    logos::Z3Converter converter(ctx);

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
            auto* lhs_if = ast_dyn_cast<IfNode>(nodes[i]);
            auto* rhs_if = ast_dyn_cast<IfNode>(nodes[i + 1]);
            if (!lhs_if || !rhs_if) continue;
            if (lhs_if->false_branch() || rhs_if->false_branch()) continue;

            Expression* lhs_expr = if_condition_expr(lhs_if);
            Expression* rhs_expr = if_condition_expr(rhs_if);
            if (!lhs_expr || !rhs_expr) continue;

            auto lhs_cond = converter.convert_to_condition(lhs_expr);
            auto rhs_cond = converter.convert_to_condition(rhs_expr);
            if (!lhs_cond.is_complementary_to(rhs_cond)) continue;

            auto* merged = arena.create<IfNode>(lhs_if->cond(), lhs_if->true_branch(), rhs_if->true_branch());
            nodes[i] = merged;
            nodes.erase(nodes.begin() + static_cast<long>(i + 1));
            changed = true;
            break;
        }
    }
}

void apply_cnf_subexpression_grouping(DecompilerArena& arena, std::vector<AstNode*>& nodes) {
    bool changed = true;
    while (changed) {
        changed = false;

        std::vector<std::size_t> candidate_indices;
        std::vector<IfNode*> candidates;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto* if_node = ast_dyn_cast<IfNode>(nodes[i]);
            if (!if_node || if_node->false_branch() || !if_condition_expr(if_node)) continue;
            candidate_indices.push_back(i);
            candidates.push_back(if_node);
        }

        if (candidates.size() < 2) return;

        ConditionCandidates graph(candidates);

        for (std::size_t i = 0; i + 1 < candidates.size(); ++i) {
            if (candidate_indices[i + 1] != candidate_indices[i] + 1) continue;

            std::string shared_clause;
            if (!graph.shared_clause(candidates[i], candidates[i + 1], &shared_clause)) continue;

            std::size_t run_end = i + 1;
            while (run_end + 1 < candidates.size() &&
                   candidate_indices[run_end + 1] == candidate_indices[run_end] + 1) {
                std::string run_clause;
                if (graph.shared_clause(candidates[i], candidates[run_end + 1], &run_clause) &&
                    run_clause == shared_clause) {
                    run_end++;
                } else {
                    break;
                }
            }

            Expression* shared_expr = graph.clause_expression(shared_clause);
            if (!shared_expr) continue;

            auto* grouped_body = arena.create<SeqNode>();
            for (std::size_t k = i; k <= run_end; ++k) {
                IfNode* old_if = candidates[k];
                Expression* old_cond = if_condition_expr(old_if);
                Expression* reduced = remove_clause_from_condition(arena, old_cond, shared_clause);

                if (!reduced) {
                    grouped_body->add_node(old_if->true_branch());
                } else {
                    auto* reduced_cond_ast = arena.create<ExprAstNode>(reduced);
                    grouped_body->add_node(arena.create<IfNode>(reduced_cond_ast, old_if->true_branch()));
                }
            }

            auto* outer_if = arena.create<IfNode>(arena.create<ExprAstNode>(shared_expr), grouped_body);

            const std::size_t start = candidate_indices[i];
            const std::size_t end = candidate_indices[run_end];
            nodes[start] = outer_if;
            nodes.erase(nodes.begin() + static_cast<long>(start + 1),
                        nodes.begin() + static_cast<long>(end + 1));

            changed = true;
            break;
        }
    }
}

bool sequence_nodes_changed(const std::vector<AstNode*>& before, const std::vector<AstNode*>& after) {
    if (before.size() != after.size()) {
        return true;
    }
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i] != after[i]) {
            return true;
        }
    }
    return false;
}

AstNode* refine_sequence_once(
    DecompilerArena& arena,
    z3::context& ctx,
    SeqNode* seq,
    const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions
) {
    if (!seq) {
        return nullptr;
    }

    std::vector<AstNode*> nodes = seq->nodes();
    if (nodes.empty()) {
        return seq;
    }

    std::vector<AstNode*> rebuilt_nodes;
    rebuilt_nodes.reserve(nodes.size() + 1);
    bool changed = false;

    for (std::size_t i = 0; i < nodes.size();) {
        AstNode* node = nodes[i];

        if (CodeNode* cnode = ast_dyn_cast<CodeNode>(node)) {
            BasicBlock* block = cnode->block();
            if (block && !block->instructions().empty()) {
                Instruction* last_inst = block->instructions().back();

                if (auto* branch = dyn_cast<Branch>(last_inst)) {
                    AstNode* branch_cond = arena.create<ExprAstNode>(branch->condition());
                    Instruction* removed_branch = last_inst;

                    auto insts = block->instructions();
                    insts.pop_back();
                    block->set_instructions(std::move(insts));

                    logos::Z3Converter conv(ctx);
                    logos::LogicCondition extracted_cond = conv.convert_to_condition(branch->condition());

                    rebuilt_nodes.push_back(node);
                    ++i;

                    const std::size_t inner_start = i;
                    SeqNode* true_branch = arena.create<SeqNode>();
                    SeqNode* false_branch = arena.create<SeqNode>();

                    while (i < nodes.size()) {
                        AstNode* next_node = nodes[i];
                        TransitionBlock* matching_tb = nullptr;

                        for (const auto& [tb, cond] : reaching_conditions) {
                            (void)cond;
                            if (tb == nullptr || next_node == nullptr) {
                                continue;
                            }
                            if (ast_node_matches_transition_block(next_node, tb)) {
                                matching_tb = tb;
                                break;
                            }
                        }

                        if (matching_tb) {
                            const auto& rc = reaching_conditions.at(matching_tb);
                            if (rc.does_imply(extracted_cond) || extracted_cond.does_imply(rc)) {
                                true_branch->add_node(next_node);
                            } else if (rc.is_complementary_to(extracted_cond)) {
                                false_branch->add_node(next_node);
                            } else {
                                if (cbr_debug_enabled()) {
                                    std::cerr << "[CBR Debug] Node " << i
                                              << " broke out! Condition did not imply/complement." << std::endl;
                                    std::cerr << "rc: " << rc.expression()
                                              << " | extracted: " << extracted_cond.expression() << std::endl;
                                }
                                break;
                            }
                        } else {
                            std::optional<bool> ast_classification =
                                classify_node_by_ast_condition(ctx, next_node, extracted_cond);
                            if (ast_classification.has_value()) {
                                if (*ast_classification) {
                                    true_branch->add_node(next_node);
                                } else {
                                    false_branch->add_node(next_node);
                                }
                            } else {
                                if (cbr_debug_enabled()) {
                                    std::cerr << "[CBR Debug] Node " << i << " broke out! No matching_tb found!" << std::endl;
                                }
                                break;
                            }
                        }
                        ++i;
                    }

                    if (!true_branch->nodes().empty() || !false_branch->nodes().empty()) {
                        IfNode* if_node = arena.create<IfNode>(
                            branch_cond,
                            true_branch,
                            false_branch->nodes().empty() ? nullptr : false_branch);
                        rebuilt_nodes.push_back(if_node);
                        changed = true;
                    } else {
                        bool synthesized_tail_if = false;

                        TransitionBlock* branch_tb = transition_block_for_ast_node(reaching_conditions, node);
                        if (branch_tb != nullptr) {
                            AstNode* true_tail = nullptr;
                            AstNode* false_tail = nullptr;

                            for (TransitionEdge* edge : branch_tb->successors()) {
                                if (!edge || !edge->sink()) {
                                    continue;
                                }

                                AstNode* succ_node = edge->sink()->ast_node();
                                if (!succ_node || succ_node == node) {
                                    continue;
                                }

                                if (!ast_node_is_terminal(succ_node)) {
                                    continue;
                                }

                                if (ast_node_appears_later(nodes, inner_start, succ_node)) {
                                    continue;
                                }

                                const logos::LogicCondition edge_cond = edge->tag();
                                if ((edge_cond.does_imply(extracted_cond) || extracted_cond.does_imply(edge_cond))
                                    && true_tail == nullptr) {
                                    true_tail = succ_node;
                                    continue;
                                }
                                if (edge_cond.is_complementary_to(extracted_cond) && false_tail == nullptr) {
                                    false_tail = succ_node;
                                }
                            }

                            if (true_tail != nullptr || false_tail != nullptr) {
                                AstNode* true_branch_node = true_tail;
                                AstNode* false_branch_node = false_tail;
                                if (true_branch_node == nullptr && false_branch_node != nullptr) {
                                    std::swap(true_branch_node, false_branch_node);
                                }

                                if (true_branch_node != nullptr) {
                                    rebuilt_nodes.push_back(arena.create<IfNode>(branch_cond, true_branch_node, false_branch_node));
                                    changed = true;
                                    synthesized_tail_if = true;
                                }
                            }
                        }

                        if (!synthesized_tail_if) {
                            auto restored = block->instructions();
                            restored.push_back(removed_branch);
                            block->set_instructions(std::move(restored));
                            const std::size_t consumed = i - inner_start;
                            i = inner_start;
                            if (cbr_debug_enabled()) {
                                std::cerr << "[CBR Debug] IfNode was empty, dropped! Rewinding "
                                          << consumed << " consumed nodes." << std::endl;
                            }
                        }
                    }

                    continue;
                }
            }
        }

        rebuilt_nodes.push_back(node);
        ++i;
    }

    const std::vector<AstNode*> before_grouping = rebuilt_nodes;
    apply_complementary_condition_pairing(arena, ctx, rebuilt_nodes);
    apply_cnf_subexpression_grouping(arena, rebuilt_nodes);

    changed = changed || sequence_nodes_changed(nodes, rebuilt_nodes)
        || sequence_nodes_changed(before_grouping, rebuilt_nodes);
    if (!changed) {
        return seq;
    }

    SeqNode* final_seq = arena.create<SeqNode>();
    for (AstNode* n : rebuilt_nodes) {
        final_seq->add_node(n);
    }
    return final_seq;
}

AstNode* refine_recursive(
    DecompilerArena& arena,
    z3::context& ctx,
    AstNode* root,
    const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions,
    std::size_t depth = 0
) {
    if (!root || depth > 64) {
        return root;
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(root)) {
        for (AstNode*& child : seq->mutable_nodes()) {
            child = refine_recursive(arena, ctx, child, reaching_conditions, depth + 1);
        }

        AstNode* current = seq;
        constexpr std::size_t kMaxLocalIterations = 4;
        for (std::size_t iter = 0; iter < kMaxLocalIterations; ++iter) {
            auto* current_seq = ast_dyn_cast<SeqNode>(current);
            if (!current_seq) {
                break;
            }

            AstNode* next = refine_sequence_once(arena, ctx, current_seq, reaching_conditions);
            if (next == current) {
                break;
            }

            current = next;
            if (auto* next_seq = ast_dyn_cast<SeqNode>(current)) {
                for (AstNode*& child : next_seq->mutable_nodes()) {
                    child = refine_recursive(arena, ctx, child, reaching_conditions, depth + 1);
                }
            }
        }
        return current;
    }

    if (auto* if_node = ast_dyn_cast<IfNode>(root)) {
        if_node->set_true_branch(refine_recursive(arena, ctx, if_node->true_branch(), reaching_conditions, depth + 1));
        if_node->set_false_branch(refine_recursive(arena, ctx, if_node->false_branch(), reaching_conditions, depth + 1));
        return if_node;
    }

    if (auto* loop_node = ast_dyn_cast<LoopNode>(root)) {
        loop_node->set_body(refine_recursive(arena, ctx, loop_node->body(), reaching_conditions, depth + 1));
        return loop_node;
    }

    if (auto* switch_node = ast_dyn_cast<SwitchNode>(root)) {
        for (CaseNode* case_node : switch_node->mutable_cases()) {
            case_node->set_body(refine_recursive(arena, ctx, case_node->body(), reaching_conditions, depth + 1));
        }
        return switch_node;
    }

    if (auto* case_node = ast_dyn_cast<CaseNode>(root)) {
        case_node->set_body(refine_recursive(arena, ctx, case_node->body(), reaching_conditions, depth + 1));
        return case_node;
    }

    return root;
}

} // namespace

namespace aletheia {

AstNode* ConditionBasedRefinement::refine(
    DecompilerArena& arena,
    z3::context& ctx,
    AstNode* root,
    const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions
) {
    return refine_recursive(arena, ctx, root, reaching_conditions);
}

} // namespace aletheia
