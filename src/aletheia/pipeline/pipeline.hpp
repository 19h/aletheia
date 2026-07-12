#pragma once
#include "../common/arena.hpp"
#include "../frontend/frontend_types.hpp"
#include "../structures/cfg.hpp"
#include "../structuring/ast.hpp"
#include "../structures/types.hpp"
#include "../../idiomata/idioms.hpp"
#include <memory>
#include <functional>
#include <optional>
#include <stdexcept>
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

    using FunctionTypeResolver = std::function<TypePtr(ida::Address)>;
    void set_function_type_resolver(FunctionTypeResolver resolver) {
        function_type_resolver_ = std::move(resolver);
    }
    TypePtr resolve_function_type(ida::Address address) const {
        return function_type_resolver_ ? function_type_resolver_(address) : nullptr;
    }

    void set_local_byte_array_extent(std::string name, std::size_t extent) {
        if (name.empty() || extent == 0) return;
        auto& current = local_byte_array_extents_[std::move(name)];
        current = std::max(current, extent);
    }
    const std::unordered_map<std::string, std::size_t>&
    local_byte_array_extents() const {
        return local_byte_array_extents_;
    }

    const std::vector<idiomata::IdiomTag>& idiom_tags() const { return idiom_tags_; }
    std::vector<idiomata::IdiomTag>& mutable_idiom_tags() { return idiom_tags_; }
    void set_idiom_tags(std::vector<idiomata::IdiomTag> tags) { idiom_tags_ = std::move(tags); }

    OutOfSsaMode out_of_ssa_mode() const { return out_of_ssa_mode_; }
    void set_out_of_ssa_mode(OutOfSsaMode mode) { out_of_ssa_mode_ = mode; }

    FrontendKind frontend_kind() const { return frontend_kind_; }
    void set_frontend_kind(FrontendKind kind) { frontend_kind_ = kind; }

    const std::vector<FrontendDiagnostic>& frontend_diagnostics() const { return frontend_diagnostics_; }
    std::vector<FrontendDiagnostic>& mutable_frontend_diagnostics() { return frontend_diagnostics_; }
    void clear_frontend_diagnostics() { frontend_diagnostics_.clear(); }
    void add_frontend_diagnostic(FrontendDiagnostic diagnostic) {
        frontend_diagnostics_.push_back(std::move(diagnostic));
    }

    const FrontendSupportReport& frontend_support_report() const { return frontend_support_report_; }
    FrontendSupportReport& mutable_frontend_support_report() { return frontend_support_report_; }
    void reset_frontend_support_report() { frontend_support_report_ = FrontendSupportReport{}; }

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
    void clear_parameter_registers() { parameter_registers_.clear(); }
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
    // The Z3 context must outlive every arena object: arena-held dataflow and
    // structuring objects may retain expressions owned by this context.
    // Members are destroyed in reverse declaration order, so keep the context
    // before the arena and the CFG/AST after it.
    z3::context z3_ctx_;
    DecompilerArena arena_;
    std::unique_ptr<ControlFlowGraph> cfg_;
    std::unique_ptr<AbstractSyntaxForest> ast_;
    std::string function_name_;
    TypePtr function_type_;
    FunctionTypeResolver function_type_resolver_;
    std::unordered_map<std::string, std::size_t> local_byte_array_extents_;
    std::vector<idiomata::IdiomTag> idiom_tags_;
    OutOfSsaMode out_of_ssa_mode_ = OutOfSsaMode::LiftMinimal;
    FrontendKind frontend_kind_ = FrontendKind::Native;
    std::unordered_map<std::string, ParameterInfo> parameter_registers_;
    std::vector<FrontendDiagnostic> frontend_diagnostics_;
    FrontendSupportReport frontend_support_report_;
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

namespace pipeline_detail {

inline constexpr std::size_t kMaxCodegenExpressionDepth = 256;
inline constexpr std::size_t kMaxCodegenAstDepth = 256;
inline constexpr std::size_t kMaxCodegenGraphExpansions = 1'000'000;

inline std::optional<std::string> expression_cycle_context(
    const char* root_name,
    const Expression* expression) {
    const auto trace = expression_graph_cycle_trace(expression);
    if (trace.empty()) return std::nullopt;
    return std::string(root_name ? root_name : "expression")
        + " contains an expression cycle of "
        + std::to_string(trace.size() - 1)
        + " edge(s)";
}

inline std::optional<std::string> instruction_cycle_context(
    const Instruction* instruction) {
    if (!instruction) return std::nullopt;
    const auto check = [](const char* name, const Expression* expression) {
        return expression_cycle_context(name, expression);
    };

    if (const auto* assignment = dyn_cast<Assignment>(instruction)) {
        if (auto cycle = check("assignment destination", assignment->destination())) {
            return cycle;
        }
        if (auto cycle = check("assignment value", assignment->value())) {
            return cycle;
        }
        if (const auto* phi = dyn_cast<Phi>(instruction)) {
            for (const auto& [_, expression] : phi->origin_block()) {
                if (auto cycle = check("phi origin", expression)) {
                    return cycle;
                }
            }
        }
        return std::nullopt;
    }
    if (const auto* relation = dyn_cast<Relation>(instruction)) {
        if (auto cycle = check("relation destination", relation->destination())) {
            return cycle;
        }
        return check("relation value", relation->value());
    }
    if (const auto* branch = dyn_cast<Branch>(instruction)) {
        return check("branch condition", branch->condition());
    }
    if (const auto* indirect = dyn_cast<IndirectBranch>(instruction)) {
        return check("indirect branch target", indirect->expression());
    }
    if (const auto* return_instruction = dyn_cast<Return>(instruction)) {
        for (const Expression* value : return_instruction->values()) {
            if (auto cycle = check("return value", value)) {
                return cycle;
            }
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> expression_codegen_complexity_context(
    const char* root_name,
    const Expression* root) {
    if (!root) return std::nullopt;
    std::vector<std::pair<const Expression*, std::size_t>> work{{root, 1}};
    std::size_t expansions = 0;
    while (!work.empty()) {
        const auto [expression, depth] = work.back();
        work.pop_back();
        if (!expression) continue;
        if (depth > kMaxCodegenExpressionDepth) {
            return std::string(root_name ? root_name : "expression")
                + " exceeds code-generation depth limit "
                + std::to_string(kMaxCodegenExpressionDepth);
        }
        if (++expansions > kMaxCodegenGraphExpansions) {
            return std::string(root_name ? root_name : "expression")
                + " exceeds code-generation expansion limit "
                + std::to_string(kMaxCodegenGraphExpansions);
        }
        auto children = expression_graph_children(expression);
        for (const Expression* child : children) {
            work.emplace_back(child, depth + 1);
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> instruction_codegen_complexity_context(
    const Instruction* instruction) {
    if (!instruction) return std::nullopt;
    const auto check = [](const char* name, const Expression* expression) {
        return expression_codegen_complexity_context(name, expression);
    };
    if (const auto* assignment = dyn_cast<Assignment>(instruction)) {
        if (auto error = check("assignment destination", assignment->destination())) {
            return error;
        }
        if (auto error = check("assignment value", assignment->value())) {
            return error;
        }
        if (const auto* phi = dyn_cast<Phi>(instruction)) {
            for (const auto& [_, expression] : phi->origin_block()) {
                if (auto error = check("phi origin", expression)) return error;
            }
        }
    } else if (const auto* relation = dyn_cast<Relation>(instruction)) {
        if (auto error = check("relation destination", relation->destination())) {
            return error;
        }
        return check("relation value", relation->value());
    } else if (const auto* branch = dyn_cast<Branch>(instruction)) {
        return check("branch condition", branch->condition());
    } else if (const auto* indirect = dyn_cast<IndirectBranch>(instruction)) {
        return check("indirect branch target", indirect->expression());
    } else if (const auto* return_instruction = dyn_cast<Return>(instruction)) {
        for (const Expression* value : return_instruction->values()) {
            if (auto error = check("return value", value)) return error;
        }
    }
    return std::nullopt;
}

inline std::optional<std::size_t> declared_parameter_count(
    const DecompilerTask& task) {
    if (!task.function_type()) return std::nullopt;
    const auto* function =
        type_dyn_cast<FunctionTypeDef>(task.function_type().get());
    if (!function) return std::nullopt;
    return function->parameters().size();
}

inline std::optional<std::string> expression_parameter_metadata_context(
    const char* root_name,
    const Expression* expression,
    std::optional<std::size_t> declared_count) {
    for (const Variable* variable : expression_graph_variables(expression)) {
        if (auto error = variable_parameter_metadata_error(
                variable, declared_count)) {
            return std::string(root_name ? root_name : "expression")
                + " variable '" + variable->name() + "_"
                + std::to_string(variable->ssa_version()) + "': " + *error;
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> instruction_parameter_metadata_context(
    const Instruction* instruction,
    std::optional<std::size_t> declared_count) {
    if (!instruction) return std::nullopt;
    const auto check = [&](const char* name, const Expression* expression) {
        return expression_parameter_metadata_context(
            name, expression, declared_count);
    };
    if (const auto* assignment = dyn_cast<Assignment>(instruction)) {
        if (auto error = check("assignment destination", assignment->destination())) {
            return error;
        }
        if (auto error = check("assignment value", assignment->value())) {
            return error;
        }
        if (const auto* phi = dyn_cast<Phi>(instruction)) {
            for (const auto& [_, expression] : phi->origin_block()) {
                if (auto error = check("phi origin", expression)) return error;
            }
        }
    } else if (const auto* relation = dyn_cast<Relation>(instruction)) {
        if (auto error = check("relation destination", relation->destination())) {
            return error;
        }
        return check("relation value", relation->value());
    } else if (const auto* branch = dyn_cast<Branch>(instruction)) {
        return check("branch condition", branch->condition());
    } else if (const auto* indirect = dyn_cast<IndirectBranch>(instruction)) {
        return check("indirect branch target", indirect->expression());
    } else if (const auto* return_instruction = dyn_cast<Return>(instruction)) {
        for (const Expression* value : return_instruction->values()) {
            if (auto error = check("return value", value)) return error;
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> cfg_parameter_metadata_context(
    const ControlFlowGraph* cfg,
    std::optional<std::size_t> declared_count = std::nullopt) {
    if (!cfg) return std::nullopt;
    for (const BasicBlock* block : cfg->blocks()) {
        if (!block) continue;
        const auto& instructions = block->instructions();
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            if (auto error = instruction_parameter_metadata_context(
                    instructions[index], declared_count)) {
                return "bb_" + std::to_string(block->id())
                    + " instruction[" + std::to_string(index) + "]: " + *error;
            }
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> cfg_cycle_context(
    const ControlFlowGraph* cfg) {
    if (!cfg) return std::nullopt;
    for (const BasicBlock* block : cfg->blocks()) {
        if (!block) continue;
        const auto& instructions = block->instructions();
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            if (auto cycle = instruction_cycle_context(instructions[index])) {
                return "bb_" + std::to_string(block->id())
                    + " instruction[" + std::to_string(index) + "]: "
                    + *cycle;
            }
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> cfg_codegen_complexity_context(
    const ControlFlowGraph* cfg) {
    if (!cfg) return std::nullopt;
    for (const BasicBlock* block : cfg->blocks()) {
        if (!block) continue;
        const auto& instructions = block->instructions();
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            if (auto error = instruction_codegen_complexity_context(
                    instructions[index])) {
                return "bb_" + std::to_string(block->id())
                    + " instruction[" + std::to_string(index) + "]: "
                    + *error;
            }
        }
    }
    return std::nullopt;
}

inline std::vector<const AstNode*> ast_children(
    const AstNode* node) {
    std::vector<const AstNode*> children;
    if (!node) return children;
    if (const auto* sequence = ast_dyn_cast<SeqNode>(node)) {
        children.reserve(sequence->nodes().size());
        for (const AstNode* child : sequence->nodes()) {
            if (child) children.push_back(child);
        }
    } else if (const auto* conditional =
                   ast_dyn_cast<IfNode>(node)) {
        if (conditional->cond()) {
            children.push_back(conditional->cond());
        }
        if (conditional->true_branch()) {
            children.push_back(conditional->true_branch());
        }
        if (conditional->false_branch()) {
            children.push_back(conditional->false_branch());
        }
    } else if (const auto* loop = ast_dyn_cast<LoopNode>(node)) {
        if (loop->body()) children.push_back(loop->body());
    } else if (const auto* case_node =
                   ast_dyn_cast<CaseNode>(node)) {
        if (case_node->body()) children.push_back(case_node->body());
    } else if (const auto* switch_node =
                   ast_dyn_cast<SwitchNode>(node)) {
        if (switch_node->cond()) {
            children.push_back(switch_node->cond());
        }
        children.reserve(children.size() + switch_node->cases().size());
        for (const CaseNode* case_node : switch_node->cases()) {
            if (case_node) children.push_back(case_node);
        }
    }
    return children;
}

inline std::optional<std::string> ast_attachment_codegen_complexity_context(
    const AstNode* node) {
    if (!node) return std::nullopt;
    if (const auto* expression = ast_dyn_cast<ExprAstNode>(node)) {
        return expression_codegen_complexity_context(
            "AST expression", expression->expr());
    }
    if (const auto* loop = ast_dyn_cast<LoopNode>(node)) {
        if (auto error = expression_codegen_complexity_context(
                "loop condition", loop->condition())) {
            return error;
        }
        if (const auto* for_loop = ast_dyn_cast<ForLoopNode>(node)) {
            if (auto error = instruction_codegen_complexity_context(
                    for_loop->declaration())) {
                return "for declaration: " + *error;
            }
            if (auto error = instruction_codegen_complexity_context(
                    for_loop->modification())) {
                return "for modification: " + *error;
            }
        }
    }
    if (const auto* code = ast_dyn_cast<CodeNode>(node);
        code && code->block()) {
        const auto& instructions = code->block()->instructions();
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            if (auto error = instruction_codegen_complexity_context(
                    instructions[index])) {
                return "code instruction[" + std::to_string(index)
                    + "]: " + *error;
            }
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> ast_codegen_complexity_context(
    const AstNode* root) {
    if (!root) return std::nullopt;
    std::vector<std::pair<const AstNode*, std::size_t>> work{{root, 1}};
    std::size_t expansions = 0;
    while (!work.empty()) {
        const auto [node, depth] = work.back();
        work.pop_back();
        if (!node) continue;
        if (depth > kMaxCodegenAstDepth) {
            return "AST exceeds code-generation depth limit "
                + std::to_string(kMaxCodegenAstDepth);
        }
        if (++expansions > kMaxCodegenGraphExpansions) {
            return "AST exceeds code-generation expansion limit "
                + std::to_string(kMaxCodegenGraphExpansions);
        }
        if (auto error = ast_attachment_codegen_complexity_context(node)) {
            return "ast-kind "
                + std::to_string(static_cast<int>(node->ast_kind()))
                + ": " + *error;
        }
        auto children = ast_children(node);
        for (const AstNode* child : children) {
            work.emplace_back(child, depth + 1);
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> ast_attachment_cycle_context(
    const AstNode* node) {
    if (!node) return std::nullopt;
    if (const auto* expression = ast_dyn_cast<ExprAstNode>(node)) {
        return expression_cycle_context(
            "AST expression", expression->expr());
    }
    if (const auto* loop = ast_dyn_cast<LoopNode>(node)) {
        if (auto cycle =
                expression_cycle_context(
                    "loop condition", loop->condition())) {
            return cycle;
        }
        if (const auto* for_loop = ast_dyn_cast<ForLoopNode>(node)) {
            if (auto cycle =
                    instruction_cycle_context(for_loop->declaration())) {
                return "for declaration: " + *cycle;
            }
            if (auto cycle =
                    instruction_cycle_context(for_loop->modification())) {
                return "for modification: " + *cycle;
            }
        }
    }
    if (const auto* code = ast_dyn_cast<CodeNode>(node);
        code && code->block()) {
        const auto& instructions = code->block()->instructions();
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            if (auto cycle =
                    instruction_cycle_context(instructions[index])) {
                return "code instruction[" + std::to_string(index)
                    + "]: " + *cycle;
            }
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> ast_cycle_context(
    const AstNode* root) {
    if (!root) return std::nullopt;
    enum class VisitState : std::uint8_t {
        Active,
        Complete,
    };
    struct Frame {
        const AstNode* node = nullptr;
        std::vector<const AstNode*> children;
        std::size_t next_child = 0;
    };

    if (auto cycle = ast_attachment_cycle_context(root)) {
        return "ast-kind " + std::to_string(
            static_cast<int>(root->ast_kind())) + ": " + *cycle;
    }

    std::unordered_map<const AstNode*, VisitState> states;
    std::unordered_map<const AstNode*, std::size_t> active_indices;
    std::vector<Frame> stack;
    states.emplace(root, VisitState::Active);
    active_indices.emplace(root, 0);
    stack.push_back(Frame{root, ast_children(root), 0});

    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next_child >= frame.children.size()) {
            states[frame.node] = VisitState::Complete;
            active_indices.erase(frame.node);
            stack.pop_back();
            continue;
        }

        const AstNode* child =
            frame.children[frame.next_child++];
        auto state = states.find(child);
        if (state != states.end()
            && state->second == VisitState::Active) {
            const std::size_t start = active_indices.at(child);
            return "AST structural cycle of "
                + std::to_string(stack.size() - start)
                + " edge(s), returning to ast-kind "
                + std::to_string(static_cast<int>(child->ast_kind()));
        }
        if (state != states.end()) continue;

        if (auto cycle = ast_attachment_cycle_context(child)) {
            return "ast-kind " + std::to_string(
                static_cast<int>(child->ast_kind())) + ": " + *cycle;
        }
        states.emplace(child, VisitState::Active);
        active_indices.emplace(child, stack.size());
        stack.push_back(Frame{child, ast_children(child), 0});
    }

    return std::nullopt;
}

inline std::optional<std::string> ast_attachment_parameter_metadata_context(
    const AstNode* node,
    std::optional<std::size_t> declared_count) {
    if (!node) return std::nullopt;
    if (const auto* expression = ast_dyn_cast<ExprAstNode>(node)) {
        return expression_parameter_metadata_context(
            "AST expression", expression->expr(), declared_count);
    }
    if (const auto* loop = ast_dyn_cast<LoopNode>(node)) {
        if (auto error = expression_parameter_metadata_context(
                "loop condition", loop->condition(), declared_count)) {
            return error;
        }
        if (const auto* for_loop = ast_dyn_cast<ForLoopNode>(node)) {
            if (auto error = instruction_parameter_metadata_context(
                    for_loop->declaration(), declared_count)) {
                return "for declaration: " + *error;
            }
            if (auto error = instruction_parameter_metadata_context(
                    for_loop->modification(), declared_count)) {
                return "for modification: " + *error;
            }
        }
    }
    if (const auto* code = ast_dyn_cast<CodeNode>(node);
        code && code->block()) {
        const auto& instructions = code->block()->instructions();
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            if (auto error = instruction_parameter_metadata_context(
                    instructions[index], declared_count)) {
                return "code instruction[" + std::to_string(index)
                    + "]: " + *error;
            }
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> ast_parameter_metadata_context(
    const AstNode* root,
    std::optional<std::size_t> declared_count) {
    if (!root) return std::nullopt;
    std::unordered_set<const AstNode*> visited;
    std::vector<const AstNode*> work{root};
    while (!work.empty()) {
        const AstNode* node = work.back();
        work.pop_back();
        if (!node || !visited.insert(node).second) continue;
        if (auto error = ast_attachment_parameter_metadata_context(
                node, declared_count)) {
            return "ast-kind "
                + std::to_string(static_cast<int>(node->ast_kind()))
                + ": " + *error;
        }
        auto children = ast_children(node);
        work.insert(work.end(), children.begin(), children.end());
    }
    return std::nullopt;
}

inline std::optional<std::string> parameter_register_metadata_context(
    const DecompilerTask& task,
    std::optional<std::size_t> declared_count) {
    for (const auto& [storage, parameter] : task.parameter_registers()) {
        if (storage.empty()) {
            return "parameter register map contains an empty storage name";
        }
        if (parameter.index < 0) {
            return "parameter register '" + storage
                + "' has a negative parameter index";
        }
        if (declared_count.has_value()
            && static_cast<std::size_t>(parameter.index) >= *declared_count) {
            return "parameter register '" + storage + "' index "
                + std::to_string(parameter.index)
                + " is outside declared parameter count "
                + std::to_string(*declared_count);
        }
        if (parameter.name.empty()) {
            return "parameter register '" + storage
                + "' has an empty display name";
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> task_parameter_metadata_context(
    const DecompilerTask& task) {
    const auto declared_count = declared_parameter_count(task);
    if (auto error = parameter_register_metadata_context(task, declared_count)) {
        return "parameter map: " + *error;
    }
    if (auto error = cfg_parameter_metadata_context(task.cfg(), declared_count)) {
        return "CFG: " + *error;
    }
    if (task.ast() && task.ast()->root()) {
        if (auto error = ast_parameter_metadata_context(
                task.ast()->root(), declared_count)) {
            return "AST: " + *error;
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> task_cycle_context(
    const DecompilerTask& task) {
    if (auto cycle = cfg_cycle_context(task.cfg())) {
        return "CFG: " + *cycle;
    }
    if (task.ast() && task.ast()->root()) {
        if (auto cycle = ast_cycle_context(task.ast()->root())) {
            return "AST: " + *cycle;
        }
    }
    return std::nullopt;
}

} // namespace pipeline_detail

// Authoritative pre-codegen structural validation. Pipeline stages permit
// temporary duplicate CodeNode ownership because callers can replace a lossy
// structured AST with a CFG fallback; emission itself requires both acyclicity
// and unique BasicBlock ownership.
inline std::optional<std::string> validate_ast_for_codegen(
    const AbstractSyntaxForest* ast) {
    if (!ast || !ast->root()) return std::nullopt;
    if (auto cycle = pipeline_detail::ast_cycle_context(ast->root())) {
        return "AST cycle: " + *cycle;
    }
    if (auto complexity =
            pipeline_detail::ast_codegen_complexity_context(ast->root())) {
        return "AST complexity: " + *complexity;
    }
    if (!ast_has_unique_code_node_ownership(ast->root())) {
        return "AST contains duplicate CodeNode ownership";
    }
    return std::nullopt;
}

inline std::optional<std::string> validate_cfg_for_codegen(
    const ControlFlowGraph* cfg) {
    if (auto cycle = pipeline_detail::cfg_cycle_context(cfg)) {
        return "CFG expression cycle: " + *cycle;
    }
    if (auto complexity =
            pipeline_detail::cfg_codegen_complexity_context(cfg)) {
        return "CFG complexity: " + *complexity;
    }
    if (auto metadata =
            pipeline_detail::cfg_parameter_metadata_context(cfg)) {
        return "CFG parameter metadata: " + *metadata;
    }
    return std::nullopt;
}

inline std::optional<std::string> validate_cfg_for_codegen(
    const DecompilerTask& task) {
    if (auto cycle = pipeline_detail::cfg_cycle_context(task.cfg())) {
        return "CFG expression cycle: " + *cycle;
    }
    if (auto complexity =
            pipeline_detail::cfg_codegen_complexity_context(task.cfg())) {
        return "CFG complexity: " + *complexity;
    }
    const auto declared_count =
        pipeline_detail::declared_parameter_count(task);
    if (auto metadata = pipeline_detail::parameter_register_metadata_context(
            task, declared_count)) {
        return "parameter map metadata: " + *metadata;
    }
    if (auto metadata = pipeline_detail::cfg_parameter_metadata_context(
            task.cfg(), declared_count)) {
        return "CFG parameter metadata: " + *metadata;
    }
    return std::nullopt;
}

inline std::optional<std::string> validate_task_for_codegen(
    const DecompilerTask& task) {
    if (auto validation = validate_cfg_for_codegen(task)) {
        return validation;
    }
    if (auto validation = validate_ast_for_codegen(task.ast())) {
        return validation;
    }
    if (task.ast() && task.ast()->root()) {
        if (auto metadata = pipeline_detail::ast_parameter_metadata_context(
                task.ast()->root(),
                pipeline_detail::declared_parameter_count(task))) {
            return "AST parameter metadata: " + *metadata;
        }
    }
    return std::nullopt;
}

class DecompilerPipeline {
public:
    DecompilerPipeline() = default;

    void add_stage(std::unique_ptr<PipelineStage> stage) {
        stages_.push_back(std::move(stage));
    }

    void run(DecompilerTask& task, const StageBoundaryObserver& observer = {}) {
        task.clear_pipeline_status();

        if (auto cycle = pipeline_detail::task_cycle_context(task)) {
            const std::string stage_name = "PipelineInput";
            task.record_stage(StageExecutionRecord{
                stage_name,
                StageExecutionStatus::Failed,
                *cycle,
            });
            task.fail_pipeline(stage_name, *cycle);
            return;
        }
        if (auto metadata =
                pipeline_detail::task_parameter_metadata_context(task)) {
            const std::string stage_name = "PipelineInput";
            const std::string message =
                "invalid parameter metadata: " + *metadata;
            task.record_stage(StageExecutionRecord{
                stage_name,
                StageExecutionStatus::Failed,
                message,
            });
            task.fail_pipeline(stage_name, message);
            return;
        }

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
                if (auto cycle =
                        pipeline_detail::task_cycle_context(task)) {
                    throw std::runtime_error(
                        "stage produced cyclic expression IR: " + *cycle);
                }
                if (auto metadata =
                        pipeline_detail::task_parameter_metadata_context(task)) {
                    throw std::runtime_error(
                        "stage produced invalid parameter metadata: "
                        + *metadata);
                }
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
