#pragma once
#include "../ast.hpp"
#include "../../../dewolf_logic/z3_logic.hpp"
#include "../reaching_conditions/reaching_conditions.hpp"
#include "../../../common/arena.hpp"

namespace dewolf {

class ConditionAwareRefinement {
public:
    static AstNode* refine(
        DecompilerArena& arena,
        z3::context& ctx,
        AstNode* root,
        const std::unordered_map<TransitionBlock*, dewolf_logic::LogicCondition>& reaching_conditions
    );
};

} // namespace dewolf
