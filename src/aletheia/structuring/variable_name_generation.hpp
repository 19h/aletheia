#pragma once

#include "ast.hpp"

namespace aletheia {

class VariableNameGeneration {
public:
    static void apply_default(AbstractSyntaxForest* forest);
    static void apply_system_hungarian(AbstractSyntaxForest* forest);
    static void apply_to_cfg(ControlFlowGraph* cfg);
};

} // namespace aletheia
