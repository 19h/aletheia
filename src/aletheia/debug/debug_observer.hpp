#pragma once
#include "../pipeline/pipeline.hpp"
#include "ir_diff.hpp"
#include "ir_invariants.hpp"
#include "stage_metrics.hpp"
#include "variable_provenance.hpp"
#include <unordered_map>
#include <unordered_set>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace aletheia::debug {

struct DebugOptions {
    bool stage_metrics = false;
    bool stage_metrics_json = false;
    bool diff_stages = false;
    std::string diff_stage_name;   // empty = all stages
    bool check_invariants = false;
    std::string check_invariants_after; // empty = all stages (if check_invariants)
    std::string trace_variable;    // empty = disabled
    bool dump_ir = false;
    bool debug_all = false;        // convenience: enables all debug flags
};

class DebugObserver {
public:
    explicit DebugObserver(DebugOptions opts, std::ostream& output);

    // StageBoundaryObserver-compatible
    void operator()(const char* stage_name, bool before_stage,
                    DecompilerTask& task);

    // Access collected data
    const StageMetricsCollector& metrics() const { return metrics_collector_; }
    const VariableProvenanceTracker& provenance() const { return provenance_tracker_; }

    // Finalize (call after pipeline + post-pipeline steps)
    std::string format_summary() const;

private:
    struct StageSelector {
        std::string stage_name;
        std::size_t ordinal = 0; // 1-based within stage name; 0 = all occurrences
        bool valid = true;
    };

    DebugOptions opts_;
    std::ostream& out_;
    StageMetricsCollector metrics_collector_;
    VariableProvenanceTracker provenance_tracker_;
    IrSnapshot last_snapshot_;
    IrInvariantChecker invariant_checker_;
    PipelinePhase current_phase_ = PipelinePhase::PreSSA;
    std::size_t stage_ordinal_ = 0;
    StageSelector invariant_selector_;
    std::unordered_map<std::string, std::size_t> stage_name_occurrence_;
    std::unordered_map<std::string, std::size_t> stage_name_total_observed_;
    std::vector<std::string> stage_name_order_;
    std::unordered_set<std::string> active_violation_fingerprints_;
    std::size_t selector_match_count_ = 0;
    mutable bool selector_diagnostic_emitted_ = false;

    void update_phase(const char* stage_name, bool after);
    static StageSelector parse_stage_selector(const std::string& raw);
    bool should_run_invariants_for_stage(const std::string& stage_name,
                                         std::size_t name_ordinal) const;
    static std::string violation_fingerprint(const InvariantViolation& violation);
    static std::string normalize_for_fingerprint(std::string_view text);
    std::string format_unknown_selector_diagnostic() const;
};

} // namespace aletheia::debug
