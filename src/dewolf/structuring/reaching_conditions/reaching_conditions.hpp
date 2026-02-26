#pragma once
#include "../transition_cfg.hpp"
#include "../../../dewolf_logic/z3_logic.hpp"
#include <unordered_map>

namespace dewolf {

class ReachingConditions {
public:
    static std::unordered_map<TransitionBlock*, dewolf_logic::LogicCondition> compute(
        z3::context& ctx,
        TransitionCFG* graph_slice,
        TransitionBlock* src,
        TransitionCFG* original_cfg
    );
};

} // namespace dewolf
