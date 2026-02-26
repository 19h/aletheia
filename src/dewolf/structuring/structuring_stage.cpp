#include "structuring_stage.hpp"

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
    }
    task.set_ast(std::move(forest));
}

void PatternIndependentRestructuringStage::build_initial_transition_cfg(DecompilerTask& task, TransitionCFG& tcfg) {
    // Map basic blocks to TransitionBlocks containing CodeNodes
}

} // namespace dewolf
