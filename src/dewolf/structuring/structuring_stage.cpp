#include "structuring_stage.hpp"
#include <unordered_map>

namespace dewolf {

void PatternIndependentRestructuringStage::execute(DecompilerTask& task) {
    TransitionCFG tcfg;
    build_initial_transition_cfg(task, tcfg);

    CyclicRegionFinder cyclic_finder(task.arena());
    cyclic_finder.process(tcfg);

    AcyclicRegionRestructurer acyclic_restructurer(task.arena());
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
    for (BasicBlock* bb : task.cfg()->blocks()) {
        TransitionBlock* src = block_map[bb];
        for (Edge* e : bb->successors()) {
            if (e->target()) {
                TransitionBlock* dst = block_map[e->target()];
                src->add_successor(dst);
                dst->add_predecessor(src);
            }
        }
    }
}

} // namespace dewolf
