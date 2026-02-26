#pragma once
#include "../pipeline/pipeline.hpp"
#include "structurer.hpp"
#include "transition_cfg.hpp"

namespace dewolf {

class PatternIndependentRestructuringStage : public PipelineStage {
public:
    const char* name() const override { return "PatternIndependentRestructuring"; }
    void execute(DecompilerTask& task) override;

private:
    void build_initial_transition_cfg(DecompilerTask& task, TransitionCFG& tcfg);
};

} // namespace dewolf
