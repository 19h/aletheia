#pragma once

#include "ast.hpp"

namespace aletheia {

class VariableNameGeneration {
public:
    static void apply_default(AbstractSyntaxForest* forest);
    static void apply_system_hungarian(AbstractSyntaxForest* forest);
};

} // namespace aletheia
