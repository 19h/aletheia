#pragma once

#include "ast.hpp"
#include "../../common/arena.hpp"

#include <cstddef>

namespace dewolf {

struct InstructionLengthBounds {
    std::size_t assignment_instr = 10;
    std::size_t call_operation = 6;
    std::size_t return_instr = 8;
};

class InstructionLengthHandler {
public:
    static void apply(
        AbstractSyntaxForest* forest,
        DecompilerArena& arena,
        InstructionLengthBounds bounds = {});
};

} // namespace dewolf
