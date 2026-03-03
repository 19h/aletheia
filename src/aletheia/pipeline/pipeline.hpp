#pragma once
#include "../common/arena.hpp"
#include "../structures/cfg.hpp"
#include "../structuring/ast.hpp"
#include "../structures/types.hpp"
#include "../../idiomata/idioms.hpp"
#include <memory>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <ida/idax.hpp>
#include <z3++.h>

namespace aletheia {

enum class OutOfSsaMode {
    Simple,
    Minimization,
    LiftMinimal,
    Conditional,
    Sreedhar,
};

enum class StageExecutionStatus {
    Success,
    Failed,
    SkippedMissingDependency,
};

struct StageExecutionRecord {
    std::string stage_name;
    StageExecutionStatus status = StageExecutionStatus::Success;
    std::string detail;
};

class DecompilerTask {
public:
    explicit DecompilerTask(ida::Address function_address)
        : function_address_(function_address) {
        // Default name
        function_name_ = "sub_" + std::to_string(function_address);
    }

    ida::Address function_address() const { return function_address_; }
    DecompilerArena& arena() { return arena_; }
    z3::context& z3_ctx() { return z3_ctx_; }
    
    ControlFlowGraph* cfg() const { return cfg_.get(); }
    void set_cfg(std::unique_ptr<ControlFlowGraph> cfg) { cfg_ = std::move(cfg); }

    AbstractSyntaxForest* ast() const { return ast_.get(); }
    void set_ast(std::unique_ptr<AbstractSyntaxForest> ast) { ast_ = std::move(ast); }

    const std::string& function_name() const { return function_name_; }
    void set_function_name(std::string name) { function_name_ = std::move(name); }

    TypePtr function_type() const { return function_type_; }
    void set_function_type(TypePtr type) { function_type_ = std::move(type); }

    const std::vector<idiomata::IdiomTag>& idiom_tags() const { return idiom_tags_; }
    std::vector<idiomata::IdiomTag>& mutable_idiom_tags() { return idiom_tags_; }
    void set_idiom_tags(std::vector<idiomata::IdiomTag> tags) { idiom_tags_ = std::move(tags); }

    OutOfSsaMode out_of_ssa_mode() const { return out_of_ssa_mode_; }
    void set_out_of_ssa_mode(OutOfSsaMode mode) { out_of_ssa_mode_ = mode; }

    const std::vector<StageExecutionRecord>& stage_records() const { return stage_records_; }
    bool failed() const { return failed_; }
    const std::string& failure_stage() const { return failure_stage_; }
    const std::string& failure_message() const { return failure_message_; }

    void clear_pipeline_status() {
        stage_records_.clear();
        failed_ = false;
        failure_stage_.clear();
        failure_message_.clear();
    }

    void record_stage(StageExecutionRecord record) {
        stage_records_.push_back(std::move(record));
    }

    void fail_pipeline(std::string stage_name, std::string message) {
        failed_ = true;
        failure_stage_ = std::move(stage_name);
        failure_message_ = std::move(message);
    }

    /// Maps register names (e.g., "rdi", "rsi", "x0") to parameter info.
    /// Populated during lifting based on function prototype and calling convention.
    struct ParameterInfo {
        std::string name;     ///< Display name (e.g., "a1", or user-defined from IDA).
        int index = -1;       ///< 0-based parameter index.
        TypePtr type;         ///< Parameter type from the prototype.
    };

    const std::unordered_map<std::string, ParameterInfo>& parameter_registers() const {
        return parameter_registers_;
    }
    void set_parameter_register(const std::string& reg_name, ParameterInfo info) {
        parameter_registers_[reg_name] = std::move(info);
    }

    /// Set of parameter display names (for filtering from local declarations).
    std::unordered_set<std::string> parameter_names() const {
        std::unordered_set<std::string> result;
        for (const auto& [reg, info] : parameter_registers_) {
            result.insert(info.name);
        }
        return result;
    }

private:
    ida::Address function_address_;
    DecompilerArena arena_;
    z3::context z3_ctx_;
    std::unique_ptr<ControlFlowGraph> cfg_;
    std::unique_ptr<AbstractSyntaxForest> ast_;
    std::string function_name_;
    TypePtr function_type_;
    std::vector<idiomata::IdiomTag> idiom_tags_;
    OutOfSsaMode out_of_ssa_mode_ = OutOfSsaMode::LiftMinimal;
    std::unordered_map<std::string, ParameterInfo> parameter_registers_;
    std::vector<StageExecutionRecord> stage_records_;
    bool failed_ = false;
    std::string failure_stage_;
    std::string failure_message_;
};

class PipelineStage {
public:
    virtual ~PipelineStage() = default;
    
    virtual const char* name() const = 0;
    virtual void execute(DecompilerTask& task) = 0;
    virtual std::vector<std::string> dependencies() const { return {}; }
};

using StageBoundaryObserver = std::function<void(
    const char* stage_name,
    bool before_stage,
    DecompilerTask& task)>;

class DecompilerPipeline {
public:
    DecompilerPipeline() = default;

    void add_stage(std::unique_ptr<PipelineStage> stage) {
        stages_.push_back(std::move(stage));
    }

    void run(DecompilerTask& task, const StageBoundaryObserver& observer = {}) {
        task.clear_pipeline_status();

        std::unordered_set<std::string> completed;
        completed.reserve(stages_.size());

        for (auto& stage : stages_) {
            const std::string stage_name = stage->name();

            std::vector<std::string> missing;
            for (const auto& dep : stage->dependencies()) {
                if (!completed.contains(dep)) {
                    missing.push_back(dep);
                }
            }

            if (!missing.empty()) {
                std::string message = "missing dependencies: ";
                for (std::size_t i = 0; i < missing.size(); ++i) {
                    if (i > 0) {
                        message += ", ";
                    }
                    message += missing[i];
                }

                task.record_stage(StageExecutionRecord{
                    stage_name,
                    StageExecutionStatus::SkippedMissingDependency,
                    message,
                });
                task.fail_pipeline(stage_name, message);
                break;
            }

            try {
                if (observer) {
                    observer(stage_name.c_str(), true, task);
                }
                stage->execute(task);
                completed.insert(stage_name);
                task.record_stage(StageExecutionRecord{
                    stage_name,
                    StageExecutionStatus::Success,
                    {},
                });
                if (observer) {
                    observer(stage_name.c_str(), false, task);
                }
            } catch (const std::exception& ex) {
                const std::string message = ex.what();
                task.record_stage(StageExecutionRecord{
                    stage_name,
                    StageExecutionStatus::Failed,
                    message,
                });
                task.fail_pipeline(stage_name, message);
                break;
            } catch (...) {
                const std::string message = "unknown exception";
                task.record_stage(StageExecutionRecord{
                    stage_name,
                    StageExecutionStatus::Failed,
                    message,
                });
                task.fail_pipeline(stage_name, message);
                break;
            }
        }
    }

private:
    std::vector<std::unique_ptr<PipelineStage>> stages_;
};

} // namespace aletheia
