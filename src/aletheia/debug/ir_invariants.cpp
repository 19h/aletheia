#include "ir_invariants.hpp"
#include "ir_serializer.hpp"
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <queue>
#include <format>
#include <optional>
#include <string_view>

namespace aletheia::debug {

namespace {

struct Arm64AliasInfo {
    char reg_class = '\0'; // 'x' or 'w'
    std::size_t number = 0;
};

std::optional<Arm64AliasInfo> parse_arm64_alias_name(std::string_view name) {
    if (name.size() < 2) {
        return std::nullopt;
    }
    const char reg_class = name[0];
    if (reg_class != 'x' && reg_class != 'w') {
        return std::nullopt;
    }
    std::size_t number = 0;
    for (std::size_t i = 1; i < name.size(); ++i) {
        const char c = name[i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        number = number * 10 + static_cast<std::size_t>(c - '0');
    }
    return Arm64AliasInfo{reg_class, number};
}

std::string variable_key(const Variable* v) {
    return v->name() + "_" + std::to_string(v->ssa_version());
}

void insert_definition_with_aliases(std::unordered_set<std::string>& defined, const Variable* v) {
    const std::string key = variable_key(v);
    defined.insert(key);

    const auto alias = parse_arm64_alias_name(v->name());
    if (!alias.has_value()) {
        return;
    }

    const char counterpart = (alias->reg_class == 'x') ? 'w' : 'x';
    const std::string alias_key = std::string(1, counterpart) + std::to_string(alias->number)
        + "_" + std::to_string(v->ssa_version());
    defined.insert(alias_key);
}

} // namespace

std::vector<InvariantViolation> IrInvariantChecker::check_all(
    const ControlFlowGraph* cfg, PipelinePhase phase) const {

    std::vector<InvariantViolation> all;

    // CFG consistency: always
    auto cfg_v = check_cfg_consistency(cfg);
    all.insert(all.end(), cfg_v.begin(), cfg_v.end());

    // SSA consistency: only during SSA phase
    if (phase == PipelinePhase::SSA) {
        auto ssa_v = check_ssa_consistency(cfg);
        all.insert(all.end(), ssa_v.begin(), ssa_v.end());
    }

    // Variable liveness: always (simplified)
    if (phase != PipelinePhase::PostStructuring) {
        auto live_v = check_variable_liveness(cfg);
        all.insert(all.end(), live_v.begin(), live_v.end());
    }

    // Call integrity: pre/post SSA
    if (phase != PipelinePhase::PostStructuring) {
        auto call_v = check_call_integrity(cfg);
        all.insert(all.end(), call_v.begin(), call_v.end());
    }

    if (phase != PipelinePhase::SSA) {
        auto ret_v = check_return_path_consistency(cfg);
        all.insert(all.end(), ret_v.begin(), ret_v.end());
    }

    return all;
}

std::vector<InvariantViolation> IrInvariantChecker::check_cfg_consistency(
    const ControlFlowGraph* cfg) const {
    std::vector<InvariantViolation> violations;
    if (!cfg) return violations;

    // 1. Edge symmetry: successor->target has this edge in predecessors
    for (auto* block : cfg->blocks()) {
        for (auto* succ_edge : block->successors()) {
            auto* target = succ_edge->target();
            bool found = false;
            for (auto* pred_edge : target->predecessors()) {
                if (pred_edge == succ_edge) { found = true; break; }
            }
            if (!found) {
                violations.push_back({
                    "edge_symmetry",
                    std::format("bb_{} -> bb_{}: successor edge not in target's predecessors",
                                block->id(), target->id()),
                    ""
                });
            }
        }
        for (auto* pred_edge : block->predecessors()) {
            auto* source = pred_edge->source();
            bool found = false;
            for (auto* succ_edge : source->successors()) {
                if (succ_edge == pred_edge) { found = true; break; }
            }
            if (!found) {
                violations.push_back({
                    "edge_symmetry",
                    std::format("bb_{} <- bb_{}: predecessor edge not in source's successors",
                                block->id(), source->id()),
                    ""
                });
            }
        }
    }

    // 2. Entry block has no predecessors
    if (cfg->entry_block() && !cfg->entry_block()->predecessors().empty()) {
        violations.push_back({
            "entry_block_no_preds",
            std::format("Entry block bb_{} has {} predecessors",
                        cfg->entry_block()->id(),
                        cfg->entry_block()->predecessors().size()),
            ""
        });
    }

    // 3. All blocks reachable from entry
    if (cfg->entry_block()) {
        std::unordered_set<const BasicBlock*> reachable;
        std::queue<const BasicBlock*> work;
        work.push(cfg->entry_block());
        reachable.insert(cfg->entry_block());
        while (!work.empty()) {
            auto* block = work.front();
            work.pop();
            for (auto* edge : block->successors()) {
                if (!reachable.contains(edge->target())) {
                    reachable.insert(edge->target());
                    work.push(edge->target());
                }
            }
        }
        for (auto* block : cfg->blocks()) {
            if (!reachable.contains(block)) {
                violations.push_back({
                    "unreachable_block",
                    std::format("bb_{} is not reachable from entry block", block->id()),
                    ""
                });
            }
        }
    }

    return violations;
}

std::vector<InvariantViolation> IrInvariantChecker::check_ssa_consistency(
    const ControlFlowGraph* cfg) const {
    std::vector<InvariantViolation> violations;
    if (!cfg) return violations;

    // Phi argument count must match predecessor count
    for (auto* block : cfg->blocks()) {
        std::size_t pred_count = block->predecessors().size();
        for (auto* inst : block->instructions()) {
            if (isa<Phi>(inst)) {
                auto* phi = cast<Phi>(inst);
                auto* oplist = phi->operand_list();
                if (oplist && oplist->size() != pred_count) {
                    violations.push_back({
                        "phi_arg_count",
                        std::format("Phi in bb_{}: {} args but {} predecessors",
                                    block->id(), oplist->size(), pred_count),
                        ir_to_string(inst)
                    });
                }
                // Check origin_block map matches if populated
                if (!phi->origin_block().empty() &&
                    phi->origin_block().size() != pred_count) {
                    violations.push_back({
                        "phi_origin_block_count",
                        std::format("Phi in bb_{}: origin_block has {} entries but {} predecessors",
                                    block->id(), phi->origin_block().size(), pred_count),
                        ir_to_string(inst)
                    });
                }
            }
        }
    }

    return violations;
}

std::vector<InvariantViolation> IrInvariantChecker::check_variable_liveness(
    const ControlFlowGraph* cfg) const {
    std::vector<InvariantViolation> violations;
    if (!cfg) return violations;

    // Collect all defined variable name+SSA pairs, all used pairs.
    // Any used pair with no definition = violation.
    // Exclusions:
    //   - GlobalVariable nodes (external symbols like _printf, string literals)
    //     are not locally defined — they represent imported/external references.
    //   - Variables with SSA version 0 and kind Register or Parameter represent
    //     implicit definitions from function entry (e.g., x0_0, sp_0, flags_0).
    std::unordered_set<std::string> defined;
    std::unordered_set<std::string> used;
    // Track which used keys are implicitly defined (global or entry-point)
    std::unordered_set<std::string> implicit;

    for (auto* block : cfg->blocks()) {
        for (auto* inst : block->instructions()) {
            std::unordered_set<Variable*> defs;
            inst->collect_definitions(defs);
            for (auto* v : defs) {
                insert_definition_with_aliases(defined, v);
            }
            std::unordered_set<Variable*> reqs;
            inst->collect_requirements(reqs);
            for (auto* v : reqs) {
                const auto key = variable_key(v);
                used.insert(key);

                // GlobalVariable: external symbol, not locally defined
                if (isa<GlobalVariable>(v)) {
                    implicit.insert(key);
                }
                // SSA version 0: implicit entry definition (registers, parameters,
                // temporaries, and stack variables all exist at function entry)
                else if (v->ssa_version() == 0) {
                    implicit.insert(key);
                }
            }
        }
    }

    for (const auto& u : used) {
        if (!defined.contains(u) && !implicit.contains(u)) {
            violations.push_back({
                "undefined_variable",
                std::format("Variable {} is used but never defined", u),
                ""
            });
        }
    }

    return violations;
}

std::vector<InvariantViolation> IrInvariantChecker::check_call_integrity(
    const ControlFlowGraph* cfg) const {
    // Basic check: all Call instructions have a non-null target
    std::vector<InvariantViolation> violations;
    if (!cfg) return violations;

    for (auto* block : cfg->blocks()) {
        for (auto* inst : block->instructions()) {
            if (isa<Assignment>(inst)) {
                auto* assign = cast<Assignment>(inst);
                if (assign->value() && isa<Call>(assign->value())) {
                    auto* call = cast<Call>(assign->value());
                    if (!call->target()) {
                        violations.push_back({
                            "call_null_target",
                            std::format("Call in bb_{} has null target", block->id()),
                            ir_to_string(inst)
                        });
                    }
                }
            }
        }
    }

    return violations;
}

std::vector<InvariantViolation> IrInvariantChecker::check_return_path_consistency(
    const ControlFlowGraph* cfg) const {
    std::vector<InvariantViolation> violations;
    if (!cfg) return violations;

    std::vector<const Return*> returns;
    for (auto* block : cfg->blocks()) {
        for (auto* inst : block->instructions()) {
            if (auto* ret = dyn_cast<Return>(inst)) {
                returns.push_back(ret);
            }
        }
    }
    if (returns.size() < 2) {
        return violations;
    }

    std::sort(returns.begin(), returns.end(), [](const Return* lhs, const Return* rhs) {
        const ida::Address la = lhs ? lhs->address() : 0;
        const ida::Address ra = rhs ? rhs->address() : 0;
        return la < ra;
    });

    auto is_param_like = [](const Variable* v) {
        if (!v) return false;
        return v->is_parameter() || v->name() == "a1" || v->name() == "arg_0";
    };

    bool seen_early_zero_constant = false;
    for (std::size_t i = 0; i + 1 < returns.size(); ++i) {
        const Return* ret = returns[i];
        if (!ret || ret->values().size() != 1) {
            continue;
        }
        auto* c = dyn_cast<Constant>(ret->values()[0]);
        if (c && c->value() == 0) {
            seen_early_zero_constant = true;
            break;
        }
    }

    const Return* terminal = returns.back();
    if (!terminal || terminal->values().size() != 1) {
        return violations;
    }
    auto* terminal_var = dyn_cast<Variable>(terminal->values()[0]);
    if (seen_early_zero_constant && is_param_like(terminal_var)) {
        violations.push_back({
            "terminal_return_path",
            "terminal return remains parameter-like while an earlier explicit zero return exists",
            ir_to_string(const_cast<Return*>(terminal))
        });
    }

    return violations;
}

std::string IrInvariantChecker::format_violations(
    const std::vector<InvariantViolation>& violations) {
    if (violations.empty()) return "";

    std::ostringstream ss;
    ss << "=== INVARIANT VIOLATIONS (" << violations.size() << ") ===\n";
    for (const auto& v : violations) {
        ss << "[" << v.invariant_name << "] " << v.description;
        if (!v.context.empty()) {
            ss << "\n  context: " << v.context;
        }
        ss << "\n";
    }
    return ss.str();
}

} // namespace aletheia::debug
