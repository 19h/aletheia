#pragma once
#include "../transition_cfg.hpp"
#include <unordered_set>
#include <vector>

namespace aletheia {

class GraphSlice {
public:
    static TransitionCFG* compute_graph_slice_for_sink_nodes(
        DecompilerArena& arena,
        TransitionCFG* t_cfg,
        TransitionBlock* source,
        const std::vector<TransitionBlock*>& sink_nodes,
        bool back_edges = true
    );

    static TransitionCFG* compute_graph_slice_for_region(
        DecompilerArena& arena,
        TransitionCFG* t_cfg,
        TransitionBlock* source,
        const std::unordered_set<TransitionBlock*>& region,
        bool back_edges = true
    );

private:
    static std::vector<TransitionBlock*> get_sink_nodes(TransitionCFG* t_cfg, const std::unordered_set<TransitionBlock*>& region);
};

} // namespace aletheia
