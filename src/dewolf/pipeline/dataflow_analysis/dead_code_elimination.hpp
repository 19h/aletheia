#pragma once
#include "../../structures/cfg.hpp"
#include "../../../common/arena.hpp"
#include "../pipeline.hpp"

namespace dewolf {

class DeadCodeEliminationStage : public PipelineStage {
public:
    const char* name() const override { return "DeadCodeElimination"; }
    void execute(DecompilerTask& task) override;
};

} // namespace dewolf
