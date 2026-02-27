#pragma once
#include "../structures/cfg.hpp"
#include "../../common/arena.hpp"
#include "../pipeline/pipeline.hpp"
#include "liveness/liveness.hpp"
#include <optional>
#include <string_view>

namespace dewolf {

class SsaDestructor : public PipelineStage {
public:
    const char* name() const override { return "SsaDestructor"; }
    void execute(DecompilerTask& task) override;

    static std::optional<OutOfSsaMode> parse_mode(std::string_view text);

private:
    void eliminate_phi_nodes(DecompilerArena& arena, ControlFlowGraph& cfg, const LivenessAnalysis& liveness);
};

} // namespace dewolf
