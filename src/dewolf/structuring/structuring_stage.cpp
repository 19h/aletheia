#include "../../dewolf_logic/z3_logic.hpp"
#include "structuring_stage.hpp"
#include <unordered_map>

namespace dewolf {

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

    // Connect edges
dewolf_logic::Z3Converter z3_conv(task.z3_ctx());
for (BasicBlock* bb : task.cfg()->blocks()) {
    TransitionBlock* src = block_map[bb];
    for (Edge* e : bb->successors()) {
        if (e->target()) {
            TransitionBlock* dst = block_map[e->target()];

            // Compute edge condition tag
            dewolf_logic::LogicCondition tag(task.z3_ctx().bool_val(true));

            if (e->type() == EdgeType::True || e->type() == EdgeType::False) {
                if (!bb->instructions().empty()) {
                    Instruction* last_inst = bb->instructions().back();
                    if (auto* branch = dynamic_cast<Branch*>(last_inst)) {
                        dewolf_logic::LogicCondition c = z3_conv.convert_to_condition(branch->condition());
                        if (e->type() == EdgeType::True) {
                            tag = c;
                        } else {
                            tag = c.negate();
                        }
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
