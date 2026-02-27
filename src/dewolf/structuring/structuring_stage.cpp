#include "../../dewolf_logic/z3_logic.hpp"
#include "structuring_stage.hpp"
#include "condition_handler.hpp"
#include <unordered_map>

namespace dewolf {

namespace {

Expression* switch_selector_expression(BasicBlock* bb) {
    if (!bb || bb->instructions().empty()) {
        return nullptr;
    }
    Instruction* tail = bb->instructions().back();
    auto* indirect = dynamic_cast<IndirectBranch*>(tail);
    if (!indirect) {
        return nullptr;
    }
    return indirect->expression();
}

std::unordered_map<Edge*, dewolf_logic::LogicCondition> build_switch_edge_tags_for_block(
    DecompilerTask& task,
    BasicBlock* bb,
    ConditionHandler& condition_handler) {
    std::unordered_map<Edge*, dewolf_logic::LogicCondition> tags;
    if (!bb) {
        return tags;
    }

    Expression* selector = switch_selector_expression(bb);
    if (!selector) {
        return tags;
    }

    const std::size_t selector_width = selector->size_bytes > 0 ? selector->size_bytes : 8;

    std::vector<Edge*> default_edges;
    z3::expr all_case_expr = task.z3_ctx().bool_val(false);

    for (Edge* edge : bb->successors()) {
        auto* switch_edge = dynamic_cast<SwitchEdge*>(edge);
        if (!switch_edge) {
            continue;
        }

        if (switch_edge->is_default()) {
            default_edges.push_back(edge);
            continue;
        }

        z3::expr edge_expr = task.z3_ctx().bool_val(false);
        if (switch_edge->case_values().empty()) {
            edge_expr = task.z3_ctx().bool_val(true);
        } else {
            for (std::int64_t case_value : switch_edge->case_values()) {
                auto* case_const = task.arena().create<Constant>(
                    static_cast<std::uint64_t>(case_value),
                    selector_width);
                auto* case_cond = task.arena().create<Condition>(OperationType::eq, selector, case_const, 1);
                auto symbol = condition_handler.add_condition(case_cond);
                edge_expr = edge_expr || symbol.expression();
            }
        }

        tags.emplace(edge, dewolf_logic::LogicCondition(edge_expr).simplify());
        all_case_expr = all_case_expr || edge_expr;
    }

    if (!default_edges.empty()) {
        z3::expr default_expr = all_case_expr.is_false()
            ? task.z3_ctx().bool_val(true)
            : !all_case_expr;
        dewolf_logic::LogicCondition default_tag(default_expr);
        for (Edge* edge : default_edges) {
            tags.insert_or_assign(edge, default_tag);
        }
    }

    // Any untagged switch edges conservatively get true.
    for (Edge* edge : bb->successors()) {
        if (edge->type() == EdgeType::Switch && !tags.contains(edge)) {
            tags.emplace(edge, dewolf_logic::LogicCondition(task.z3_ctx().bool_val(true)));
        }
    }

    return tags;
}

} // namespace

void PatternIndependentRestructuringStage::execute(DecompilerTask& task) {
    TransitionCFG tcfg(task.arena());
    build_initial_transition_cfg(task, tcfg);

    CyclicRegionFinder cyclic_finder(task);
    cyclic_finder.process(tcfg);

    AcyclicRegionRestructurer acyclic_restructurer(task);
    acyclic_restructurer.process(tcfg);

    auto forest = std::make_unique<AbstractSyntaxForest>();
    if (tcfg.entry()) {
        forest->set_root(tcfg.entry()->ast_node());
    } else if (task.cfg() && task.cfg()->entry_block()) {
        // Fallback for visualization if tcfg wasn't fully structured
        // Just wrap the entry block in a CodeNode
        CodeNode* root = task.arena().create<CodeNode>(task.cfg()->entry_block());
        forest->set_root(root);
    }
    task.set_ast(std::move(forest));
}

void PatternIndependentRestructuringStage::build_initial_transition_cfg(DecompilerTask& task, TransitionCFG& tcfg) {
    if (!task.cfg()) return;

    std::unordered_map<BasicBlock*, TransitionBlock*> block_map;

    // Create a TransitionBlock for every BasicBlock
    for (BasicBlock* bb : task.cfg()->blocks()) {
        CodeNode* cnode = task.arena().create<CodeNode>(bb);
        TransitionBlock* tb = task.arena().create<TransitionBlock>(cnode);
        block_map[bb] = tb;

        if (bb == task.cfg()->entry_block()) {
            tcfg.set_entry(tb);
        }
        tcfg.add_block(tb);
    }

    // Connect edges with condition-symbol tags.
    ConditionHandler condition_handler(task.z3_ctx());
    for (BasicBlock* bb : task.cfg()->blocks()) {
        TransitionBlock* src = block_map[bb];
        const auto switch_tags = build_switch_edge_tags_for_block(task, bb, condition_handler);
        for (Edge* e : bb->successors()) {
            if (e->target()) {
                TransitionBlock* dst = block_map[e->target()];

                dewolf_logic::LogicCondition tag(task.z3_ctx().bool_val(true));
                if (auto switch_it = switch_tags.find(e); switch_it != switch_tags.end()) {
                    tag = switch_it->second;
                } else if (e->type() == EdgeType::True || e->type() == EdgeType::False) {
                    if (!bb->instructions().empty()) {
                        Instruction* last_inst = bb->instructions().back();
                        if (auto* branch = dynamic_cast<Branch*>(last_inst)) {
                            dewolf_logic::LogicCondition symbol_cond = condition_handler.add_condition(branch->condition());
                            tag = (e->type() == EdgeType::True) ? symbol_cond : symbol_cond.negate();
                        }
                    }
                }

                std::optional<EdgeProperty> prop = std::nullopt;
                if (task.cfg()->edge_properties().contains(e)) {
                    prop = task.cfg()->edge_properties().at(e);
                }

                tcfg.add_edge(src, dst, tag, prop);
            }
        }
    }

}

} // namespace dewolf
