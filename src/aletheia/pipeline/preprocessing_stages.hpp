#pragma once
#include "pipeline.hpp"

namespace aletheia {

class CompilerIdiomHandlingStage : public PipelineStage {
public:
    const char* name() const override { return "CompilerIdiomHandling"; }
    void execute(DecompilerTask& task) override;
};

class RegisterPairHandlingStage : public PipelineStage {
public:
    const char* name() const override { return "RegisterPairHandling"; }
    void execute(DecompilerTask& task) override;
};

class SwitchVariableDetectionStage : public PipelineStage {
public:
    const char* name() const override { return "SwitchVariableDetection"; }
    void execute(DecompilerTask& task) override;
};

class MemPhiConverterStage : public PipelineStage {
public:
    const char* name() const override { return "MemPhiConverter"; }
    void execute(DecompilerTask& task) override;
};

class RemoveGoPrologueStage : public PipelineStage {
public:
    const char* name() const override { return "RemoveGoPrologue"; }
    void execute(DecompilerTask& task) override;
};

class RemoveStackCanaryStage : public PipelineStage {
public:
    const char* name() const override { return "RemoveStackCanary"; }
    void execute(DecompilerTask& task) override;
};

class RemoveNoreturnBoilerplateStage : public PipelineStage {
public:
    const char* name() const override { return "RemoveNoreturnBoilerplate"; }
    void execute(DecompilerTask& task) override;
};

class InsertMissingDefinitionsStage : public PipelineStage {
public:
    const char* name() const override { return "InsertMissingDefinitions"; }
    void execute(DecompilerTask& task) override;
};

class PhiFunctionFixerStage : public PipelineStage {
public:
    const char* name() const override { return "PhiFunctionFixer"; }
    void execute(DecompilerTask& task) override;
    std::vector<std::string> dependencies() const override { return {"SsaConstructor"}; }
};

class EmptyBasicBlockRemoverStage : public PipelineStage {
public:
    const char* name() const override { return "EmptyBasicBlockRemover"; }
    void execute(DecompilerTask& task) override;
};

class CoherenceStage : public PipelineStage {
public:
    const char* name() const override { return "Coherence"; }
    void execute(DecompilerTask& task) override;
};

class FallthroughBlockMergeStage : public PipelineStage {
public:
    const char* name() const override { return "FallthroughBlockMerge"; }
    void execute(DecompilerTask& task) override;
};

/// Pre-SSA local constant folding within basic blocks.
///
/// Tracks the most recent definition of each variable within a block and
/// substitutes Constant values forward into uses. This folds ADRP+ADD
/// pairs (e.g., X1 = Const(page); X1 = X1 + Const(offset)) into a single
/// constant assignment (X1 = Const(page + offset)), and inlines constants
/// into Call arguments so that downstream AddressResolution can resolve
/// them to string literals and symbol names.
///
/// Must run BEFORE SSA construction. Operates only on straight-line code
/// within each basic block — no cross-block analysis required.
class LocalConstantFoldingStage : public PipelineStage {
public:
    const char* name() const override { return "LocalConstantFolding"; }
    void execute(DecompilerTask& task) override;
};

} // namespace aletheia
