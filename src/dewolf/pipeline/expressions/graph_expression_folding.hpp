#pragma once
#include "../../structures/cfg.hpp"
#include "../../../common/arena.hpp"
#include "../pipeline.hpp"

namespace dewolf {

class GraphExpressionFoldingStage : public PipelineStage {
public:
    const char* name() const override { return "GraphExpressionFolding"; }
    void execute(DecompilerTask& task) override;
};

} // namespace dewolf
