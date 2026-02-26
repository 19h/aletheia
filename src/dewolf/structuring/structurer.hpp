#pragma once
#include "transition_cfg.hpp"
#include "../../common/arena.hpp"

namespace dewolf {

class CyclicRegionFinder {
public:
    explicit CyclicRegionFinder(DecompilerArena& arena) : arena_(arena) {}
    
    // Reduces cycles in the TransitionCFG into LoopNode constructs
    void process(TransitionCFG& cfg);

private:
    DecompilerArena& arena_;

    void detect_back_edges(TransitionCFG& cfg);
    void formulate_loop_bodies();
    void synthesize_breaks_and_continues();
};

class AcyclicRegionRestructurer {
public:
    explicit AcyclicRegionRestructurer(DecompilerArena& arena) : arena_(arena) {}

    // Reduces acyclic branches in the TransitionCFG into IfNode/SwitchNode constructs
    void process(TransitionCFG& cfg);

private:
    DecompilerArena& arena_;
};

} // namespace dewolf
