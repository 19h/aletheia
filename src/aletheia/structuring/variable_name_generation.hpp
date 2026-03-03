#pragma once

#include "ast.hpp"

namespace aletheia {

class VariableNameGeneration {
public:
    static void apply_default(AbstractSyntaxForest* forest);
    static void apply_system_hungarian(AbstractSyntaxForest* forest);
    static void apply_to_cfg(ControlFlowGraph* cfg);

    /// Remove self-assignments (dst = dst) that become visible after
    /// variable renaming collapses different SSA versions to the same name.
    static void remove_self_assignments(ControlFlowGraph* cfg);
};

} // namespace aletheia
