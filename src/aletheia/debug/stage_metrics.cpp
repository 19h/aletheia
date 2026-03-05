#include "stage_metrics.hpp"
#include <sstream>
#include <iomanip>
#include <format>
#include <string_view>
#include <unordered_set>

namespace aletheia::debug {

namespace {

std::string escape_json(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

void append_stage_metrics_json(std::ostringstream& ss, const StageMetrics& m) {
    ss << "{\"ordinal\": " << m.stage_ordinal
       << ", \"name\": \"" << escape_json(m.stage_name) << "\""
       << ", \"duration_us\": " << m.duration_us
       << ", \"instructions_before\": " << m.instructions_before
       << ", \"instructions_after\": " << m.instructions_after
       << ", \"variables_before\": " << m.variables_before
       << ", \"variables_after\": " << m.variables_after
       << ", \"blocks_before\": " << m.blocks_before
       << ", \"blocks_after\": " << m.blocks_after
       << ", \"failed\": " << (m.failed ? "true" : "false")
       << "}";
}

} // namespace

std::size_t StageMetricsCollector::count_instructions(const ControlFlowGraph* cfg) {
    if (!cfg) return 0;
    std::size_t count = 0;
    for (auto* block : cfg->blocks()) {
        count += block->instructions().size();
    }
    return count;
}

std::size_t StageMetricsCollector::count_variables(const ControlFlowGraph* cfg) {
    if (!cfg) return 0;
    std::unordered_set<std::string> var_names;
    for (auto* block : cfg->blocks()) {
        for (auto* inst : block->instructions()) {
            std::unordered_set<Variable*> defs;
            inst->collect_definitions(defs);
            for (auto* v : defs) {
                var_names.insert(v->name() + "_" + std::to_string(v->ssa_version()));
            }
            std::unordered_set<Variable*> reqs;
            inst->collect_requirements(reqs);
            for (auto* v : reqs) {
                var_names.insert(v->name() + "_" + std::to_string(v->ssa_version()));
            }
        }
    }
    return var_names.size();
}

std::size_t StageMetricsCollector::count_blocks(const ControlFlowGraph* cfg) {
    if (!cfg) return 0;
    return cfg->blocks().size();
}

void StageMetricsCollector::before_stage(const char* stage_name, const DecompilerTask& task) {
    // Handle orphaned previous stage
    if (stage_in_flight_) {
        pending_.failed = true;
        pending_.duration_us = 0;
        metrics_.push_back(pending_);
    }

    pending_ = StageMetrics{};
    pending_.stage_name = stage_name;
    pending_.stage_ordinal = next_ordinal_++;
    pending_.instructions_before = count_instructions(task.cfg());
    pending_.variables_before = count_variables(task.cfg());
    pending_.blocks_before = count_blocks(task.cfg());

    stage_start_ = std::chrono::high_resolution_clock::now();
    stage_in_flight_ = true;
}

void StageMetricsCollector::after_stage(const char* stage_name, const DecompilerTask& task) {
    auto end = std::chrono::high_resolution_clock::now();

    if (!stage_in_flight_) return; // Shouldn't happen, but be safe

    pending_.duration_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - stage_start_).count());
    pending_.instructions_after = count_instructions(task.cfg());
    pending_.variables_after = count_variables(task.cfg());
    pending_.blocks_after = count_blocks(task.cfg());
    pending_.failed = false;

    metrics_.push_back(pending_);
    stage_in_flight_ = false;
}

void StageMetricsCollector::finalize_pending() {
    if (stage_in_flight_) {
        pending_.failed = true;
        pending_.duration_us = 0;
        metrics_.push_back(pending_);
        stage_in_flight_ = false;
    }
}

std::string StageMetricsCollector::format_table() const {
    std::ostringstream ss;
    ss << std::left << std::setw(4) << "#"
       << std::setw(45) << "Stage"
       << std::right << std::setw(10) << "Time(us)"
       << std::setw(8) << "Insts"
       << std::setw(8) << "Vars"
       << std::setw(8) << "Blocks"
       << "\n";
    ss << std::string(83, '-') << "\n";

    std::uint64_t total_us = 0;
    for (const auto& m : metrics_) {
        ss << std::left << std::setw(4) << m.stage_ordinal
           << std::setw(45) << m.stage_name
           << std::right << std::setw(10);
        if (m.failed) {
            ss << "FAILED";
        } else {
            ss << m.duration_us;
        }
        ss << std::setw(8) << m.instructions_after
           << std::setw(8) << m.variables_after
           << std::setw(8) << m.blocks_after
           << "\n";
        total_us += m.duration_us;
    }

    ss << std::string(83, '-') << "\n";
    ss << std::left << std::setw(49) << "    TOTAL"
       << std::right << std::setw(10) << total_us
       << "\n";
    return ss.str();
}

std::string StageMetricsCollector::format_json() const {
    std::ostringstream ss;
    ss << "[\n";
    for (std::size_t i = 0; i < metrics_.size(); ++i) {
        const auto& m = metrics_[i];
        ss << "  ";
        append_stage_metrics_json(ss, m);
        if (i + 1 < metrics_.size()) ss << ",";
        ss << "\n";
    }
    ss << "]\n";
    return ss.str();
}

std::string format_stage_metrics_report_json(
    std::string_view input_binary,
    std::size_t total_functions,
    std::size_t decompiled_functions,
    const std::vector<FunctionStageMetrics>& functions) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"input_binary\": \"" << escape_json(input_binary) << "\",\n";
    ss << "  \"total_functions\": " << total_functions << ",\n";
    ss << "  \"decompiled_functions\": " << decompiled_functions << ",\n";
    ss << "  \"functions\": [\n";
    for (std::size_t i = 0; i < functions.size(); ++i) {
        const auto& fn = functions[i];
        ss << "    {\n";
        ss << "      \"name\": \"" << escape_json(fn.function_name) << "\",\n";
        ss << "      \"address\": " << fn.function_address << ",\n";
        ss << "      \"stages\": [\n";
        for (std::size_t j = 0; j < fn.stages.size(); ++j) {
            ss << "        ";
            append_stage_metrics_json(ss, fn.stages[j]);
            if (j + 1 < fn.stages.size()) {
                ss << ",";
            }
            ss << "\n";
        }
        ss << "      ]\n";
        ss << "    }";
        if (i + 1 < functions.size()) {
            ss << ",";
        }
        ss << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

} // namespace aletheia::debug
