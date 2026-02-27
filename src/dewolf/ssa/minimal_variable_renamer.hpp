#pragma once

#include "../structures/cfg.hpp"
#include "../../common/arena.hpp"

namespace dewolf {

class MinimalVariableRenamer {
public:
    static void rename(DecompilerArena& arena, ControlFlowGraph& cfg);
};

} // namespace dewolf
