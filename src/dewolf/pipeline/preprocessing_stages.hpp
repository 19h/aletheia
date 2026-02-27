#pragma once
#include "pipeline.hpp"

namespace dewolf {

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
};

class CoherenceStage : public PipelineStage {
public:
    const char* name() const override { return "Coherence"; }
    void execute(DecompilerTask& task) override;
};

} // namespace dewolf
