#pragma once
#include "../pipeline/pipeline.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace aletheia::debug {

struct ProvenanceEvent {
    std::string stage_name;
    std::size_t stage_ordinal = 0;
    std::string description;
    std::string variable_name;
    std::size_t ssa_version = 0;
    VariableKind kind = VariableKind::Register;
    int parameter_index = -1;
};

class VariableProvenanceTracker {
public:
    void before_stage(const char* stage_name, std::size_t ordinal,
                      const DecompilerTask& task);
    void after_stage(const char* stage_name, std::size_t ordinal,
                     const DecompilerTask& task);

    std::vector<ProvenanceEvent> trace_variable(const std::string& name) const;
    std::string format_trace(const std::string& name) const;

private:
    struct VarInfo {
        std::string name;
        std::size_t ssa_version;
        VariableKind kind;
        int parameter_index;
        std::int64_t stack_offset;
        bool aliased;
    };
    struct StageSnapshot {
        std::string stage_name;
        std::size_t stage_ordinal;
        std::vector<VarInfo> variables;
    };
    std::vector<StageSnapshot> snapshots_;
    std::vector<ProvenanceEvent> events_;

    void collect_variables(const ControlFlowGraph* cfg, std::vector<VarInfo>& out) const;
    void detect_changes(const StageSnapshot& before, const StageSnapshot& after);
};

} // namespace aletheia::debug
