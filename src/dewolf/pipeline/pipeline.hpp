#pragma once
#include "../common/arena.hpp"
#include "../structures/cfg.hpp"
#include "../structuring/ast.hpp"
#include <memory>
#include <ida/idax.hpp>
#include <z3++.h>

namespace dewolf {

class DecompilerTask {
public:
    explicit DecompilerTask(ida::Address function_address)
        : function_address_(function_address) {}

    ida::Address function_address() const { return function_address_; }
    DecompilerArena& arena() { return arena_; }
    z3::context& z3_ctx() { return z3_ctx_; }
    
    ControlFlowGraph* cfg() const { return cfg_.get(); }
    void set_cfg(std::unique_ptr<ControlFlowGraph> cfg) { cfg_ = std::move(cfg); }

    AbstractSyntaxForest* ast() const { return ast_.get(); }
    void set_ast(std::unique_ptr<AbstractSyntaxForest> ast) { ast_ = std::move(ast); }

private:
    ida::Address function_address_;
    DecompilerArena arena_;
    z3::context z3_ctx_;
    std::unique_ptr<ControlFlowGraph> cfg_;
    std::unique_ptr<AbstractSyntaxForest> ast_;
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
