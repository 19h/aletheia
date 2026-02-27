#pragma once
#include "../common/arena.hpp"
#include "../structures/cfg.hpp"
#include "../structuring/ast.hpp"
#include "../structures/types.hpp"
#include <memory>
#include <string>
#include <ida/idax.hpp>
#include <z3++.h>

namespace dewolf {

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

private:
    ida::Address function_address_;
    DecompilerArena arena_;
    z3::context z3_ctx_;
    std::unique_ptr<ControlFlowGraph> cfg_;
    std::unique_ptr<AbstractSyntaxForest> ast_;
    std::string function_name_;
    TypePtr function_type_;
};

class PipelineStage {
public:
    virtual ~PipelineStage() = default;
    
    virtual const char* name() const = 0;
    virtual void execute(DecompilerTask& task) = 0;
};

class DecompilerPipeline {
public:
    DecompilerPipeline() = default;

    void add_stage(std::unique_ptr<PipelineStage> stage) {
        stages_.push_back(std::move(stage));
    }

    void run(DecompilerTask& task) {
        for (auto& stage : stages_) {
            stage->execute(task);
        }
    }

private:
    std::vector<std::unique_ptr<PipelineStage>> stages_;
};

} // namespace dewolf
