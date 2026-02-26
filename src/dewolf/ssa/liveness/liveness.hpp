#pragma once
#include "../../structures/cfg.hpp"
#include "../../structures/dataflow.hpp"
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace dewolf {

class LivenessAnalysis {
public:
    explicit LivenessAnalysis(ControlFlowGraph& cfg);

    const std::unordered_set<std::string>& live_in(BasicBlock* block) const;
    const std::unordered_set<std::string>& live_out(BasicBlock* block) const;
    const std::unordered_set<std::string>& defs_phi(BasicBlock* block) const;

private:
    ControlFlowGraph& cfg_;

    std::unordered_map<BasicBlock*, std::unordered_set<std::string>> uses_block_;
    std::unordered_map<BasicBlock*, std::unordered_set<std::string>> defs_block_;
    std::unordered_map<BasicBlock*, std::unordered_set<std::string>> uses_phi_block_;
    std::unordered_map<BasicBlock*, std::unordered_set<std::string>> defs_phi_block_;

    std::unordered_map<BasicBlock*, std::unordered_set<std::string>> live_in_block_;
    std::unordered_map<BasicBlock*, std::unordered_set<std::string>> live_out_block_;

    void create_live_sets();
    void init_usages_definitions_of_blocks();
    void explore_all_paths(BasicBlock* block, const std::string& variable);

    // Helpers to extract uses/defs from expressions
    void extract_vars(Expression* expr, std::unordered_set<std::string>& out);
};

} // namespace dewolf
