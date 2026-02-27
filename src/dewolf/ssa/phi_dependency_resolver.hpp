#pragma once
#include "../structures/cfg.hpp"
#include "../../common/arena.hpp"

namespace dewolf {

class PhiDependencyResolver {
public:
    static void resolve(DecompilerArena& arena, ControlFlowGraph& cfg);
};

} // namespace dewolf
