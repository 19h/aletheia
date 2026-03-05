#pragma once
#include "../structures/cfg.hpp"
#include <string>
#include <vector>

namespace aletheia::debug {

enum class PipelinePhase {
    PreSSA,          // Before SsaConstructor
    SSA,             // Between SsaConstructor and SsaDestructor
    PostSSA,         // After SsaDestructor, before structuring
    PostStructuring  // After PatternIndependentRestructuring
};

struct InvariantViolation {
    std::string invariant_name;
    std::string description;
    std::string context;
};

class IrInvariantChecker {
public:
    // Check all phase-appropriate invariants
    std::vector<InvariantViolation> check_all(
        const ControlFlowGraph* cfg, PipelinePhase phase) const;

    // Individual checks
    std::vector<InvariantViolation> check_cfg_consistency(const ControlFlowGraph* cfg) const;
    std::vector<InvariantViolation> check_ssa_consistency(const ControlFlowGraph* cfg) const;
    std::vector<InvariantViolation> check_variable_liveness(const ControlFlowGraph* cfg) const;
    std::vector<InvariantViolation> check_call_integrity(const ControlFlowGraph* cfg) const;
    std::vector<InvariantViolation> check_return_path_consistency(const ControlFlowGraph* cfg) const;

    static std::string format_violations(const std::vector<InvariantViolation>& violations);
};

} // namespace aletheia::debug
