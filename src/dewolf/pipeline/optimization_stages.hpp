#pragma once
#include "pipeline.hpp"

namespace dewolf {

class ExpressionPropagationStage : public PipelineStage {
public:
    const char* name() const override { return "ExpressionPropagation"; }
    void execute(DecompilerTask& task) override;
};

class ExpressionPropagationMemoryStage : public PipelineStage {
public:
    const char* name() const override { return "ExpressionPropagationMemory"; }
    void execute(DecompilerTask& task) override;
};

class ExpressionPropagationFunctionCallStage : public PipelineStage {
public:
    const char* name() const override { return "ExpressionPropagationFunctionCall"; }
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

class DeadLoopEliminationStage : public PipelineStage {
public:
    const char* name() const override { return "DeadLoopElimination"; }
    void execute(DecompilerTask& task) override;
};

class IdentityEliminationStage : public PipelineStage {
public:
    const char* name() const override { return "IdentityElimination"; }
    void execute(DecompilerTask& task) override;
};

class ExpressionSimplificationStage : public PipelineStage {
public:
    const char* name() const override { return "ExpressionSimplification"; }
    void execute(DecompilerTask& task) override;
};

class DeadComponentPrunerStage : public PipelineStage {
public:
    const char* name() const override { return "DeadComponentPruner"; }
    void execute(DecompilerTask& task) override;
};

class RedundantCastsEliminationStage : public PipelineStage {
public:
    const char* name() const override { return "RedundantCastsElimination"; }
    void execute(DecompilerTask& task) override;
};

class ArrayAccessDetectionStage : public PipelineStage {
public:
    const char* name() const override { return "ArrayAccessDetection"; }
    void execute(DecompilerTask& task) override;
};

class EdgePrunerStage : public PipelineStage {
public:
    const char* name() const override { return "EdgePruner"; }
    void execute(DecompilerTask& task) override;
};

class CommonSubexpressionEliminationStage : public PipelineStage {
public:
    const char* name() const override { return "CommonSubexpressionElimination"; }
    void execute(DecompilerTask& task) override;
};

} // namespace dewolf
