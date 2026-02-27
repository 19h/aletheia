#include "cbr.hpp"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <vector>

namespace {

using namespace dewolf;

Expression* if_condition_expr(IfNode* node) {
    if (!node) return nullptr;
    if (auto* expr_ast = dynamic_cast<ExprAstNode*>(node->cond())) {
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

void flatten_conjunction(Expression* expr, std::vector<Expression*>& out) {
    if (!expr) return;
    auto* op = dynamic_cast<Operation*>(expr);
    if (op && op->type() == OperationType::logical_and && op->operands().size() == 2) {
        flatten_conjunction(op->operands()[0], out);
        flatten_conjunction(op->operands()[1], out);
        return;
    }
    out.push_back(expr);
}

void flatten_disjunction(Expression* expr, std::vector<Expression*>& out) {
    if (!expr) return;
    auto* op = dynamic_cast<Operation*>(expr);
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
    dewolf_logic::Z3Converter converter(ctx);

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
            auto* lhs_if = dynamic_cast<IfNode*>(nodes[i]);
            auto* rhs_if = dynamic_cast<IfNode*>(nodes[i + 1]);
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
            auto* if_node = dynamic_cast<IfNode*>(nodes[i]);
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

} // namespace

namespace dewolf {

AstNode* ConditionBasedRefinement::refine(
    DecompilerArena& arena,
    z3::context& ctx,
    AstNode* root,
    const std::unordered_map<TransitionBlock*, dewolf_logic::LogicCondition>& reaching_conditions
) {
    if (auto* seq = dynamic_cast<SeqNode*>(root)) {
        std::vector<AstNode*> nodes = seq->nodes();
        if (nodes.empty()) return root;

        SeqNode* new_seq = arena.create<SeqNode>();

        for (size_t i = 0; i < nodes.size(); ) {
            AstNode* node = nodes[i];
            
            AstNode* branch_cond = nullptr;
            if (CodeNode* cnode = dynamic_cast<CodeNode*>(node)) {
                if (!cnode->block()->instructions().empty()) {
                    Instruction* last_inst = cnode->block()->instructions().back();
                    
                    // Check if the last instruction is a Branch (conditional)
                    if (auto* branch = dynamic_cast<Branch*>(last_inst)) {
                        branch_cond = arena.create<ExprAstNode>(branch->condition());
                        
                        // Remove the branch instruction from the block
                        auto insts = cnode->block()->instructions();
                        insts.pop_back();
                        cnode->block()->set_instructions(std::move(insts));
                        
                        dewolf_logic::Z3Converter conv(ctx);
                        dewolf_logic::LogicCondition extracted_cond = conv.convert_to_condition(branch->condition());
                        
                        new_seq->add_node(node);
                        i++;

                        SeqNode* true_branch = arena.create<SeqNode>();
                        SeqNode* false_branch = arena.create<SeqNode>();
                        
                        while (i < nodes.size()) {
                            AstNode* next_node = nodes[i];
                            TransitionBlock* matching_tb = nullptr;
                            for (const auto& [tb, cond] : reaching_conditions) {
                                if (tb->ast_node() == next_node || tb->ast_node()->get_original_block() == next_node->get_original_block()) {
                                    matching_tb = tb;
                                    break;
                                }
                            }

                            if (matching_tb) {
                                auto& rc = reaching_conditions.at(matching_tb);
                                if (rc.does_imply(extracted_cond) || extracted_cond.does_imply(rc)) {
                                    true_branch->add_node(next_node);
                                } else if (rc.is_complementary_to(extracted_cond)) {
                                    false_branch->add_node(next_node);
                                } else {
                                    std::cerr << "[CBR Debug] Node " << i << " broke out! Condition did not imply/complement." << std::endl;
                                    std::cerr << "rc: " << rc.expression() << " | extracted: " << extracted_cond.expression() << std::endl;
                                    break;
                                }
                            } else {
                                std::cerr << "[CBR Debug] Node " << i << " broke out! No matching_tb found!" << std::endl;
                                break;
                            }
                            i++;
                        }

                        if (!true_branch->nodes().empty() || !false_branch->nodes().empty()) {
                            IfNode* if_node = arena.create<IfNode>(branch_cond, true_branch, false_branch->nodes().empty() ? nullptr : false_branch);
                            new_seq->add_node(if_node);
                        } else {
                            std::cerr << "[CBR Debug] IfNode was empty, dropped!" << std::endl;
                        }
                        
                        continue;
                    }
                }
            }

            new_seq->add_node(node);
            i++;
        }
        
        std::vector<AstNode*> refined_nodes = new_seq->nodes();

        apply_complementary_condition_pairing(arena, ctx, refined_nodes);
        apply_cnf_subexpression_grouping(arena, refined_nodes);

        SeqNode* final_seq = arena.create<SeqNode>();
        for (AstNode* n : refined_nodes) {
            final_seq->add_node(n);
        }

        return final_seq;
    }

    return root;
}

} // namespace dewolf
