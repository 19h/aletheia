#pragma once
#include "transition_cfg.hpp"
#include "../../common/arena.hpp"
#include "../pipeline/pipeline.hpp"
#include <unordered_set>

namespace aletheia {

class CyclicRegionFinder {
public:
    explicit CyclicRegionFinder(DecompilerTask& task) : task_(task), arena_(task.arena()) {}
    
    // Reduces cycles in the TransitionCFG into LoopNode constructs
    void process(TransitionCFG& cfg);

private:
    DecompilerTask& task_;
    DecompilerArena& arena_;

    void detect_back_edges(TransitionCFG& cfg);
    void formulate_loop_bodies();
    void synthesize_breaks_and_continues();
};

class AcyclicRegionRestructurer {
public:
    explicit AcyclicRegionRestructurer(DecompilerTask& task) : task_(task), arena_(task.arena()) {}

    // Reduces acyclic branches in the TransitionCFG into IfNode/SwitchNode constructs
    void process(TransitionCFG& cfg);

private:
    DecompilerTask& task_;
    DecompilerArena& arena_;

    // Internal restructuring matching python DeWolf
    void restructure_region(TransitionCFG* t_cfg, TransitionBlock* header, const std::unordered_set<TransitionBlock*>& region);
};

} // namespace aletheia
