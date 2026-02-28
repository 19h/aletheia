#pragma once
#include "../transition_cfg.hpp"
#include "../../../logos/z3_logic.hpp"
#include <unordered_map>

namespace aletheia {

class ReachingConditions {
public:
    static std::unordered_map<TransitionBlock*, logos::LogicCondition> compute(
        z3::context& ctx,
        TransitionCFG* graph_slice,
        TransitionBlock* src,
        TransitionCFG* original_cfg
    );
};

} // namespace aletheia
