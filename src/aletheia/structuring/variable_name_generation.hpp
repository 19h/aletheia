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

    /// Same as above, but traverses the AST tree to find CodeNode blocks.
    /// This is needed because the structured AST path may wrap blocks that
    /// are not (or no longer) in the flat CFG block list.
    static void remove_self_assignments_ast(AbstractSyntaxForest* forest);
};

} // namespace aletheia
