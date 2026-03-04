#pragma once
#include "pipeline.hpp"

namespace aletheia {

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

/// Validates that variables used by Return instructions have at least one
/// dominating reaching definition (or are parameters). Fails the pipeline when
/// this invariant is violated.
class ReturnDefinitionSanityStage : public PipelineStage {
public:
    const char* name() const override { return "ReturnDefinitionSanity"; }
    void execute(DecompilerTask& task) override;
};

/// Repairs missing/non-dominating definitions for variables consumed by
/// branch conditions and return values by inserting conservative SSA bridge
/// assignments before the sink use sites.
class SinkDefinitionRepairStage : public PipelineStage {
public:
    const char* name() const override { return "SinkDefinitionRepair"; }
    void execute(DecompilerTask& task) override;
};

/// For functions explicitly typed as void, clear accidental return values in
/// CFG Return nodes so downstream codegen does not infer a non-void signature
/// from disconnected temporaries.
class VoidReturnNormalizationStage : public PipelineStage {
public:
    const char* name() const override { return "VoidReturnNormalization"; }
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

class RedundantAssignmentEliminationStage : public PipelineStage {
public:
    const char* name() const override { return "RedundantAssignmentElimination"; }
    void execute(DecompilerTask& task) override;
};

class CommonSubexpressionEliminationStage : public PipelineStage {
public:
    const char* name() const override { return "CommonSubexpressionElimination"; }
    void execute(DecompilerTask& task) override;
};


class AstExpressionSimplificationStage : public PipelineStage {
public:
    const char* name() const override { return "AstExpressionSimplification"; }
    void execute(DecompilerTask& task) override;
};

/// Resolves remaining address-valued Constants to string literals or named
/// symbols using IDA's database.  Runs AFTER constant folding so that
/// ADRP+ADD pairs have already been collapsed to a single address.
class AddressResolutionStage : public PipelineStage {
public:
    const char* name() const override { return "AddressResolution"; }
    void execute(DecompilerTask& task) override;
};

} // namespace aletheia
