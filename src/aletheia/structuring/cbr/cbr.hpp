#pragma once
#include "../ast.hpp"
#include "../../../logos/z3_logic.hpp"
#include "../reaching_conditions/reaching_conditions.hpp"
#include "../../../common/arena.hpp"

namespace aletheia {

class ConditionBasedRefinement {
public:
    static AstNode* refine(
        DecompilerArena& arena,
        z3::context& ctx,
        AstNode* root,
        const std::unordered_map<TransitionBlock*, logos::LogicCondition>& reaching_conditions
    );
};

} // namespace aletheia
