#pragma once
#include "dominators.hpp"
#include "../structures/cfg.hpp"
#include "../structures/dataflow.hpp"
#include "../../common/arena.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace dewolf {

class SsaConstructor {
public:
    explicit SsaConstructor(DecompilerArena& arena, ControlFlowGraph& cfg)
        : arena_(arena), cfg_(cfg), dom_tree_(cfg) {}

    void run();

private:
    void insert_phi_nodes();
    void rename_variables();

    DecompilerArena& arena_;
    ControlFlowGraph& cfg_;
    DominatorTree dom_tree_;

    // Variables per block definition tracking
    std::unordered_map<std::string, std::vector<BasicBlock*>> var_defs_;
    std::unordered_map<BasicBlock*, std::vector<Instruction*>> phi_nodes_;
    
    void gather_definitions();
};

} // namespace dewolf
