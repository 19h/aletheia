#pragma once

#include "ast.hpp"

namespace aletheia {

class LoopNameGenerator {
public:
    static void apply_for_loop_counters(AbstractSyntaxForest* forest);
    static void apply_while_loop_counters(AbstractSyntaxForest* forest);
};

} // namespace aletheia
