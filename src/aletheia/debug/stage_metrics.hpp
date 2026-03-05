#pragma once
#include "../pipeline/pipeline.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aletheia::debug {

struct StageMetrics {
    std::string stage_name;
    std::size_t stage_ordinal = 0;       // 1-indexed, unique per invocation
    std::uint64_t duration_us = 0;
    std::size_t instructions_before = 0;
    std::size_t instructions_after = 0;
    std::size_t variables_before = 0;
    std::size_t variables_after = 0;
    std::size_t blocks_before = 0;
    std::size_t blocks_after = 0;
    bool failed = false;
};

class StageMetricsCollector {
public:
    void before_stage(const char* stage_name, const DecompilerTask& task);
    void after_stage(const char* stage_name, const DecompilerTask& task);
    const std::vector<StageMetrics>& metrics() const { return metrics_; }
    std::string format_table() const;
    std::string format_json() const;

    // Handle orphaned before_stage (stage threw exception)
    void finalize_pending();

private:
    std::vector<StageMetrics> metrics_;
    std::size_t next_ordinal_ = 1;
    std::chrono::high_resolution_clock::time_point stage_start_;
    bool stage_in_flight_ = false;
    StageMetrics pending_;

    // Counting helpers
    static std::size_t count_instructions(const ControlFlowGraph* cfg);
    static std::size_t count_variables(const ControlFlowGraph* cfg);
    static std::size_t count_blocks(const ControlFlowGraph* cfg);
};

struct FunctionStageMetrics {
    std::string function_name;
    std::uint64_t function_address = 0;
    std::vector<StageMetrics> stages;
};

std::string format_stage_metrics_report_json(
    std::string_view input_binary,
    std::size_t total_functions,
    std::size_t decompiled_functions,
    const std::vector<FunctionStageMetrics>& functions);

} // namespace aletheia::debug
