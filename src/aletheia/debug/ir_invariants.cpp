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

std::string expression_label(const Expression* expression) {
    if (!expression) return "null";
    switch (expression->node_kind()) {
    case NodeKind::Constant:
        return std::format(
            "constant(0x{:x})",
            static_cast<const Constant*>(expression)->value());
    case NodeKind::Variable:
        return std::format(
            "variable({}_{})",
            static_cast<const Variable*>(expression)->name(),
            static_cast<const Variable*>(expression)->ssa_version());
    case NodeKind::GlobalVariable:
        return std::format(
            "global({})",
            static_cast<const GlobalVariable*>(expression)->name());
    case NodeKind::ListOperation:
        return "list";
    case NodeKind::Operation:
    case NodeKind::Call:
    case NodeKind::Condition:
        return std::format(
            "operation({})",
            static_cast<int>(
                static_cast<const Operation*>(expression)->type()));
    default:
        return std::format(
            "expression-kind({})",
            static_cast<int>(expression->node_kind()));
    }
}

struct NamedExpressionRoot {
    std::string name;
    const Expression* expression = nullptr;
};

std::vector<NamedExpressionRoot> instruction_expression_roots(
    const Instruction* instruction) {
    std::vector<NamedExpressionRoot> roots;
    if (!instruction) return roots;

    if (const auto* assignment = dyn_cast<Assignment>(instruction)) {
        roots.push_back({"destination", assignment->destination()});
        roots.push_back({"value", assignment->value()});
        if (const auto* phi = dyn_cast<Phi>(instruction)) {
            for (const auto& [origin, expression] : phi->origin_block()) {
                roots.push_back({
                    origin
                        ? std::format("phi-origin-bb_{}", origin->id())
                        : "phi-origin-null",
                    expression,
                });
            }
        }
    } else if (const auto* relation = dyn_cast<Relation>(instruction)) {
        roots.push_back({"destination", relation->destination()});
        roots.push_back({"value", relation->value()});
    } else if (const auto* branch = dyn_cast<Branch>(instruction)) {
        roots.push_back({"condition", branch->condition()});
    } else if (const auto* indirect =
                   dyn_cast<IndirectBranch>(instruction)) {
        roots.push_back({"target", indirect->expression()});
    } else if (const auto* return_instruction =
                   dyn_cast<Return>(instruction)) {
        for (std::size_t index = 0;
             index < return_instruction->values().size();
             ++index) {
            roots.push_back({
                std::format("return-value[{}]", index),
                return_instruction->values()[index],
            });
        }
    }
    return roots;
}

} // namespace

std::vector<InvariantViolation> IrInvariantChecker::check_all(
    const ControlFlowGraph* cfg,
    PipelinePhase phase,
    std::optional<std::size_t> declared_parameter_count) const {

    std::vector<InvariantViolation> all;

    // CFG consistency: always
    auto cfg_v = check_cfg_consistency(cfg);
    all.insert(all.end(), cfg_v.begin(), cfg_v.end());

    // Expression cycles can make requirement collection, cloning, and code
    // generation recurse indefinitely. Detect them before invoking any other
    // expression walker and return the safe diagnostics immediately.
    auto expression_v = check_expression_acyclicity(cfg);
    all.insert(all.end(), expression_v.begin(), expression_v.end());
    if (!expression_v.empty()) {
        return all;
    }

    auto parameter_v = check_parameter_metadata(
        cfg, declared_parameter_count);
    all.insert(all.end(), parameter_v.begin(), parameter_v.end());

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

std::vector<InvariantViolation> IrInvariantChecker::check_parameter_metadata(
    const ControlFlowGraph* cfg,
    std::optional<std::size_t> declared_parameter_count) const {
    std::vector<InvariantViolation> violations;
    if (!cfg) return violations;

    std::unordered_set<const Variable*> reported;
    for (const BasicBlock* block : cfg->blocks()) {
        if (!block) continue;
        const auto& instructions = block->instructions();
        for (std::size_t instruction_index = 0;
             instruction_index < instructions.size();
             ++instruction_index) {
            const Instruction* instruction = instructions[instruction_index];
            for (const NamedExpressionRoot& root :
                 instruction_expression_roots(instruction)) {
                for (const Variable* variable :
                     expression_graph_variables(root.expression)) {
                    if (!variable || isa<GlobalVariable>(variable)
                        || reported.contains(variable)) {
                        continue;
                    }
                    auto error = variable_parameter_metadata_error(
                        variable, declared_parameter_count);
                    if (!error) continue;
                    reported.insert(variable);
                    violations.push_back({
                        "parameter_metadata",
                        std::format(
                            "bb_{} instruction[{}] root '{}' variable {}_{}: {}",
                            block->id(),
                            instruction_index,
                            root.name,
                            variable->name(),
                            variable->ssa_version(),
                            *error),
                        std::format(
                            "kind={} parameter_index={} address=0x{:x}",
                            static_cast<int>(variable->kind()),
                            variable->parameter_index(),
                            instruction ? instruction->address() : 0),
                    });
                }
            }
        }
    }
    return violations;
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

std::vector<InvariantViolation>
IrInvariantChecker::check_expression_acyclicity(
    const ControlFlowGraph* cfg) const {
    std::vector<InvariantViolation> violations;
    if (!cfg) return violations;

    for (const BasicBlock* block : cfg->blocks()) {
        if (!block) continue;
        const auto& instructions = block->instructions();
        for (std::size_t instruction_index = 0;
             instruction_index < instructions.size();
             ++instruction_index) {
            const Instruction* instruction =
                instructions[instruction_index];
            for (const NamedExpressionRoot& root :
                 instruction_expression_roots(instruction)) {
                const std::vector<const Expression*> trace =
                    expression_graph_cycle_trace(root.expression);
                if (trace.empty()) {
                    continue;
                }

                std::string path;
                for (std::size_t index = 0;
                     index < trace.size();
                     ++index) {
                    if (index > 0) path += " -> ";
                    path += expression_label(trace[index]);
                }
                violations.push_back({
                    "expression_cycle",
                    std::format(
                        "bb_{} instruction[{}] root '{}' contains cycle: {}",
                        block->id(),
                        instruction_index,
                        root.name,
                        path),
                    std::format(
                        "instruction-kind={} address=0x{:x}",
                        instruction
                            ? static_cast<int>(instruction->node_kind())
                            : -1,
                        instruction ? instruction->address() : 0),
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
