#include "variable_provenance.hpp"
#include "ir_serializer.hpp"
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <format>

namespace aletheia::debug {

void VariableProvenanceTracker::collect_variables(
    const ControlFlowGraph* cfg, std::vector<VarInfo>& out) const {
    if (!cfg) return;

    std::unordered_set<std::string> seen;
    for (auto* block : cfg->blocks()) {
        for (auto* inst : block->instructions()) {
            std::unordered_set<Variable*> defs;
            inst->collect_definitions(defs);
            for (auto* v : defs) {
                std::string key = v->name() + "_" + std::to_string(v->ssa_version());
                if (!seen.contains(key)) {
                    seen.insert(key);
                    out.push_back({
                        v->name(),
                        v->ssa_version(),
                        v->kind(),
                        v->parameter_index(),
                        v->stack_offset(),
                        v->is_aliased()
                    });
                }
            }
            std::unordered_set<Variable*> reqs;
            inst->collect_requirements(reqs);
            for (auto* v : reqs) {
                std::string key = v->name() + "_" + std::to_string(v->ssa_version());
                if (!seen.contains(key)) {
                    seen.insert(key);
                    out.push_back({
                        v->name(),
                        v->ssa_version(),
                        v->kind(),
                        v->parameter_index(),
                        v->stack_offset(),
                        v->is_aliased()
                    });
                }
            }
        }
    }
}

void VariableProvenanceTracker::before_stage(
    const char* stage_name, std::size_t ordinal,
    const DecompilerTask& task) {
    StageSnapshot snap;
    snap.stage_name = stage_name;
    snap.stage_ordinal = ordinal;
    collect_variables(task.cfg(), snap.variables);
    snapshots_.push_back(std::move(snap));
}

void VariableProvenanceTracker::after_stage(
    const char* stage_name, std::size_t ordinal,
    const DecompilerTask& task) {
    StageSnapshot after_snap;
    after_snap.stage_name = stage_name;
    after_snap.stage_ordinal = ordinal;
    collect_variables(task.cfg(), after_snap.variables);

    // Detect changes between the last before_stage snapshot and this after snapshot
    if (!snapshots_.empty()) {
        detect_changes(snapshots_.back(), after_snap);
    }

    // Replace the last snapshot with the after-state
    if (!snapshots_.empty()) {
        snapshots_.back() = std::move(after_snap);
    }
}

void VariableProvenanceTracker::detect_changes(
    const StageSnapshot& before, const StageSnapshot& after) {
    // Build maps: name_ssa -> VarInfo
    std::unordered_map<std::string, const VarInfo*> before_map;
    std::unordered_map<std::string, const VarInfo*> after_map;

    for (auto& v : before.variables) {
        std::string key = v.name + "_" + std::to_string(v.ssa_version);
        before_map[key] = &v;
    }
    for (auto& v : after.variables) {
        std::string key = v.name + "_" + std::to_string(v.ssa_version);
        after_map[key] = &v;
    }

    // Variables that disappeared
    for (auto& [key, info] : before_map) {
        if (!after_map.contains(key)) {
            events_.push_back({
                after.stage_name,
                after.stage_ordinal,
                std::format("Variable {} removed", key),
                info->name,
                info->ssa_version,
                info->kind,
                info->parameter_index
            });
        }
    }

    // Variables that appeared
    for (auto& [key, info] : after_map) {
        if (!before_map.contains(key)) {
            events_.push_back({
                after.stage_name,
                after.stage_ordinal,
                std::format("Variable {} created", key),
                info->name,
                info->ssa_version,
                info->kind,
                info->parameter_index
            });
        }
    }

    // Variables whose kind changed
    for (auto& [key, after_info] : after_map) {
        auto it = before_map.find(key);
        if (it != before_map.end()) {
            auto* before_info = it->second;
            if (before_info->kind != after_info->kind) {
                bool provenance_loss = (before_info->kind == VariableKind::Parameter &&
                                        after_info->kind != VariableKind::Parameter);
                std::string desc = std::format("Variable {} kind changed: {} -> {}",
                                                key,
                                                variable_kind_name(before_info->kind),
                                                variable_kind_name(after_info->kind));
                if (provenance_loss) {
                    desc += " *** PROVENANCE LOSS: Parameter kind dropped ***";
                }
                events_.push_back({
                    after.stage_name,
                    after.stage_ordinal,
                    desc,
                    after_info->name,
                    after_info->ssa_version,
                    after_info->kind,
                    after_info->parameter_index
                });
            }
        }
    }
}

std::vector<ProvenanceEvent> VariableProvenanceTracker::trace_variable(
    const std::string& name) const {
    std::vector<ProvenanceEvent> result;
    for (const auto& evt : events_) {
        // Match by base name (without SSA version)
        if (evt.variable_name == name) {
            result.push_back(evt);
        }
    }
    return result;
}

std::string VariableProvenanceTracker::format_trace(const std::string& name) const {
    auto events = trace_variable(name);
    if (events.empty()) {
        return std::format("No provenance events for variable '{}'\n", name);
    }

    std::ostringstream ss;
    ss << "=== Variable Provenance: " << name << " ===\n";
    for (const auto& evt : events) {
        ss << std::format("[{:02}/{} {}] {}",
                          evt.stage_ordinal,
                          snapshots_.empty() ? 0 : snapshots_.back().stage_ordinal,
                          evt.stage_name,
                          evt.description)
           << "\n";
    }
    return ss.str();
}

} // namespace aletheia::debug
