#pragma once

#include "../structures/cfg.hpp"
#include "../../common/arena.hpp"

namespace aletheia {

class ConditionalVariableRenamer {
public:
    static void rename(DecompilerArena& arena, ControlFlowGraph& cfg);
};

} // namespace aletheia
