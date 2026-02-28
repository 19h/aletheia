#pragma once
#include "dominators.hpp"
#include "../structures/cfg.hpp"
#include "../structures/dataflow.hpp"
#include "../../common/arena.hpp"
#include "../pipeline/pipeline.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace aletheia {

class SsaConstructor : public PipelineStage {
public:
    const char* name() const override { return "SsaConstructor"; }
    void execute(DecompilerTask& task) override;

private:
    void insert_phi_nodes(DecompilerArena& arena, ControlFlowGraph& cfg, const DominatorTree& dom_tree);
    void rename_variables(DecompilerArena& arena, ControlFlowGraph& cfg, const DominatorTree& dom_tree);

    /// Maps variable name -> list of blocks that contain a definition for it.
    std::unordered_map<std::string, std::vector<BasicBlock*>> var_defs_;
    
    /// Maps block -> phi instructions placed at that block.
    std::unordered_map<BasicBlock*, std::vector<Phi*>> phi_nodes_;
    
    void gather_definitions(ControlFlowGraph& cfg);
};

} // namespace aletheia
