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

namespace dewolf {

class SsaConstructor : public PipelineStage {
public:
    const char* name() const override { return "SsaConstructor"; }
    void execute(DecompilerTask& task) override;

private:
    void insert_phi_nodes(DecompilerArena& arena, ControlFlowGraph& cfg, const DominatorTree& dom_tree);
    void rename_variables(DecompilerArena& arena, ControlFlowGraph& cfg, const DominatorTree& dom_tree);

    std::unordered_map<std::string, std::vector<BasicBlock*>> var_defs_;
    std::unordered_map<BasicBlock*, std::vector<Instruction*>> phi_nodes_;
    
    void gather_definitions(ControlFlowGraph& cfg);
};

} // namespace dewolf
