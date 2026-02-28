#pragma once
#include "../structures/cfg.hpp"
#include "../../common/arena.hpp"
#include "../pipeline/pipeline.hpp"
#include "liveness/liveness.hpp"
#include <optional>
#include <string_view>

namespace aletheia {

class SsaDestructor : public PipelineStage {
public:
    const char* name() const override { return "SsaDestructor"; }
    void execute(DecompilerTask& task) override;
    std::vector<std::string> dependencies() const override { return {"SsaConstructor"}; }

    static std::optional<OutOfSsaMode> parse_mode(std::string_view text);

private:
    void eliminate_phi_nodes(DecompilerArena& arena, ControlFlowGraph& cfg, const LivenessAnalysis& liveness);
};

} // namespace aletheia
