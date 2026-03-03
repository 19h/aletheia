#include "../../logos/z3_logic.hpp"
#include "structuring_stage.hpp"
#include "condition_handler.hpp"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_map>

namespace aletheia {

namespace {

bool env_flag_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    std::string_view v{value};
    return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
}

std::uint64_t basic_block_order_key(BasicBlock* bb) {
    if (!bb) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(bb->id());
}

std::vector<BasicBlock*> sorted_basic_blocks_by_id(const std::vector<BasicBlock*>& blocks) {
    std::vector<BasicBlock*> sorted = blocks;
    std::stable_sort(sorted.begin(), sorted.end(), [](BasicBlock* lhs, BasicBlock* rhs) {
        return basic_block_order_key(lhs) < basic_block_order_key(rhs);
    });
    return sorted;
}

std::vector<Edge*> sorted_successor_edges(const std::vector<Edge*>& edges) {
    std::vector<Edge*> sorted = edges;
    std::stable_sort(sorted.begin(), sorted.end(), [](Edge* lhs, Edge* rhs) {
        BasicBlock* lhs_target = lhs ? lhs->target() : nullptr;
        BasicBlock* rhs_target = rhs ? rhs->target() : nullptr;

        const std::uint64_t lhs_key = basic_block_order_key(lhs_target);
        const std::uint64_t rhs_key = basic_block_order_key(rhs_target);
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key;
        }

        const int lhs_type = lhs ? static_cast<int>(lhs->type()) : std::numeric_limits<int>::max();
        const int rhs_type = rhs ? static_cast<int>(rhs->type()) : std::numeric_limits<int>::max();
        if (lhs_type != rhs_type) {
            return lhs_type < rhs_type;
        }

        const int lhs_kind = lhs ? static_cast<int>(lhs->edge_kind()) : std::numeric_limits<int>::max();
        const int rhs_kind = rhs ? static_cast<int>(rhs->edge_kind()) : std::numeric_limits<int>::max();
        if (lhs_kind != rhs_kind) {
            return lhs_kind < rhs_kind;
        }

        return false;
    });
    return sorted;
}

Expression* switch_selector_expression(BasicBlock* bb) {
    if (!bb || bb->instructions().empty()) {
        return nullptr;
    }
    Instruction* tail = bb->instructions().back();
    auto* indirect = dyn_cast<IndirectBranch>(tail);
    if (!indirect) {
        return nullptr;
    }
    return indirect->expression();
}

std::unordered_map<Edge*, logos::LogicCondition> build_switch_edge_tags_for_block(
    DecompilerTask& task,
    BasicBlock* bb,
    ConditionHandler& condition_handler) {
    std::unordered_map<Edge*, logos::LogicCondition> tags;
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

    for (Edge* edge : sorted_successor_edges(bb->successors())) {
        auto* switch_edge = (edge && edge->edge_kind() == EdgeKind::SwitchEdge ? static_cast<SwitchEdge*>(edge) : nullptr);
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

        tags.emplace(edge, logos::LogicCondition(edge_expr).simplify());
        all_case_expr = all_case_expr || edge_expr;
    }

    if (!default_edges.empty()) {
        z3::expr default_expr = all_case_expr.is_false()
            ? task.z3_ctx().bool_val(true)
            : !all_case_expr;
        logos::LogicCondition default_tag(default_expr);
        for (Edge* edge : default_edges) {
            tags.insert_or_assign(edge, default_tag);
        }
    }

    // Any untagged switch edges conservatively get true.
    for (Edge* edge : sorted_successor_edges(bb->successors())) {
        if (edge->type() == EdgeType::Switch && !tags.contains(edge)) {
            tags.emplace(edge, logos::LogicCondition(task.z3_ctx().bool_val(true)));
        }
    }

    return tags;
}

} // namespace

void PatternIndependentRestructuringStage::execute(DecompilerTask& task) {
    const bool trace = env_flag_enabled("ALETHEIA_STRUCT_TRACE");
    const bool disable_cyclic = env_flag_enabled("ALETHEIA_DISABLE_CYCLIC");
    const bool disable_acyclic = env_flag_enabled("ALETHEIA_DISABLE_ACYCLIC");

    if (trace) {
        std::cerr << "[Struct] begin execute\n";
    }

    TransitionCFG tcfg(task.arena());
    build_initial_transition_cfg(task, tcfg);
    if (trace) {
        std::cerr << "[Struct] built initial transition cfg\n";
    }

    if (!disable_cyclic) {
        CyclicRegionFinder cyclic_finder(task);
        cyclic_finder.process(tcfg);
        if (trace) {
            std::cerr << "[Struct] finished cyclic finder\n";
        }
    }

    if (!disable_acyclic) {
        AcyclicRegionRestructurer acyclic_restructurer(task);
        acyclic_restructurer.process(tcfg);
        if (trace) {
            std::cerr << "[Struct] finished acyclic restructurer\n";
        }
    }


    auto forest = std::make_unique<AbstractSyntaxForest>();
    if (tcfg.entry()) {
        if (getenv("DEBUG_ENTRY")) {
            std::cerr << "tcfg.entry() AST node present? " << (tcfg.entry()->ast_node() != nullptr) << std::endl;
            std::cerr << "tcfg blocks size: " << tcfg.blocks().size() << std::endl;
        }
        forest->set_root(tcfg.entry()->ast_node());
    } else if (task.cfg() && task.cfg()->entry_block()) {
        // Fallback for visualization if tcfg wasn't fully structured
        // Just wrap the entry block in a CodeNode
        CodeNode* root = task.arena().create<CodeNode>(task.cfg()->entry_block());
        forest->set_root(root);
    }
    task.set_ast(std::move(forest));

    if (trace) {
        std::cerr << "[Struct] end execute\n";
    }
}

void PatternIndependentRestructuringStage::build_initial_transition_cfg(DecompilerTask& task, TransitionCFG& tcfg) {
    if (!task.cfg()) return;

    std::unordered_map<BasicBlock*, TransitionBlock*> block_map;

    // Create a TransitionBlock for every BasicBlock
    const std::vector<BasicBlock*> ordered_blocks = sorted_basic_blocks_by_id(task.cfg()->blocks());

    for (BasicBlock* bb : ordered_blocks) {
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
    for (BasicBlock* bb : ordered_blocks) {
        TransitionBlock* src = block_map[bb];
        const auto switch_tags = build_switch_edge_tags_for_block(task, bb, condition_handler);
        for (Edge* e : sorted_successor_edges(bb->successors())) {
            if (e->target()) {
                TransitionBlock* dst = block_map[e->target()];

                logos::LogicCondition tag(task.z3_ctx().bool_val(true));
                if (auto switch_it = switch_tags.find(e); switch_it != switch_tags.end()) {
                    tag = switch_it->second;
                } else if (e->type() == EdgeType::True || e->type() == EdgeType::False) {
                    if (!bb->instructions().empty()) {
                        Instruction* last_inst = bb->instructions().back();
                        if (auto* branch = dyn_cast<Branch>(last_inst)) {
                            logos::Z3Converter z3conv(task.z3_ctx());
                            logos::LogicCondition symbol_cond = z3conv.convert_to_condition(branch->condition());
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

} // namespace aletheia
