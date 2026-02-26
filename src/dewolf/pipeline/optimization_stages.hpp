#pragma once
#include "pipeline.hpp"

namespace dewolf {

class ExpressionPropagationStage : public PipelineStage {
public:
    const char* name() const override { return "ExpressionPropagation"; }
    void execute(DecompilerTask& task) override;
};

class TypePropagationStage : public PipelineStage {
public:
    const char* name() const override { return "TypePropagation"; }
    void execute(DecompilerTask& task) override;
};

class BitFieldComparisonUnrollingStage : public PipelineStage {
public:
    const char* name() const override { return "BitFieldComparisonUnrolling"; }
    void execute(DecompilerTask& task) override;
};

class DeadPathEliminationStage : public PipelineStage {
public:
    const char* name() const override { return "DeadPathElimination"; }
    void execute(DecompilerTask& task) override;
};

class CommonSubexpressionEliminationStage : public PipelineStage {
public:
    const char* name() const override { return "CommonSubexpressionElimination"; }
    void execute(DecompilerTask& task) override;
};

} // namespace dewolf
