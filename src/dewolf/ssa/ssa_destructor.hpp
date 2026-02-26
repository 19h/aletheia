#pragma once
#include "../structures/cfg.hpp"
#include "../../common/arena.hpp"
#include "liveness/liveness.hpp"

namespace dewolf {

class SsaDestructor {
public:
    explicit SsaDestructor(DecompilerArena& arena, ControlFlowGraph& cfg)
        : arena_(arena), cfg_(cfg) {}

    void run();

private:
    DecompilerArena& arena_;
    ControlFlowGraph& cfg_;

    void eliminate_phi_nodes(const LivenessAnalysis& liveness);
};

} // namespace dewolf
