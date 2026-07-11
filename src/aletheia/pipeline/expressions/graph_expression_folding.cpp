#include "graph_expression_folding.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aletheia {

// Variable identity key for SSA-form variables.
// Variables in SSA are uniquely identified by (name, ssa_version), not pointer identity.
// After SSA construction, definitions and uses are separate Variable* objects that share
// the same name and SSA version but are different pointers. Using pointer identity as
// map keys causes substitution lookups to fail silently.

struct VarKey {
    std::string name;
    std::size_t ssa_version;

    bool operator==(const VarKey& other) const {
        return name == other.name && ssa_version == other.ssa_version;
    }
};

struct VarKeyHash {
    std::size_t operator()(const VarKey& k) const {
        return std::hash<std::string>{}(k.name) ^
               (std::hash<std::size_t>{}(k.ssa_version) << 16);
    }
};

static VarKey var_key(Variable* v) {
    return {v->name(), v->ssa_version()};
}

static bool is_float_arithmetic(OperationType type) {
    return type == OperationType::add_float
        || type == OperationType::sub_float
        || type == OperationType::mul_float
        || type == OperationType::div_float;
}

static Expression* copy_for_use(Expression* replacement,
                                Variable* use,
                                DecompilerArena& arena,
                                bool floating_context = false) {
    if (!replacement) {
        return nullptr;
    }
    Expression* copy = replacement->copy(arena);
    if (floating_context) {
        copy->set_ir_type(std::make_shared<const Float>(copy->size_bytes * 8));
    } else if (use && use->ir_type()) {
        // A raw P-Code varnode can have a storage definition and a different
        // per-use interpretation (notably IEEE-754 bits in FLOAT_* ops).
        // Preserve the use-site interpretation when folding the storage copy.
        copy->set_ir_type(use->ir_type());
    }
    return copy;
}

static void substitute_uses(Expression* expr,
                            const std::unordered_map<VarKey, Expression*, VarKeyHash>& subs,
                            DecompilerArena& arena) {
    if (!expr) return;
    if (auto* op = dyn_cast<Operation>(expr)) {
        const bool floating_context = is_float_arithmetic(op->type());
        for (size_t i = 0; i < op->operands().size(); ++i) {
            if (auto* v = dyn_cast<Variable>(op->operands()[i])) {
                auto it = subs.find(var_key(v));
                if (it != subs.end()) {
                    op->mutable_operands()[i] = copy_for_use(
                        it->second, v, arena, floating_context);
                }
            } else {
                substitute_uses(op->operands()[i], subs, arena);
            }
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (size_t i = 0; i < list->operands().size(); ++i) {
            if (auto* v = dyn_cast<Variable>(list->operands()[i])) {
                auto it = subs.find(var_key(v));
                if (it != subs.end()) {
                    list->mutable_operands()[i] = copy_for_use(it->second, v, arena);
                }
            } else {
                substitute_uses(list->operands()[i], subs, arena);
            }
        }
    }
}

void GraphExpressionFoldingStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    auto is_unstable_version_zero = [](const Variable* var) {
        if (!var) {
            return false;
        }
        if (var->ssa_version() != 0) {
            return false;
        }
        return var->kind() == VariableKind::Register
            || var->kind() == VariableKind::Parameter;
    };

    // True Global Expression Graph Folding
    // Identity mapping Phase — uses VarKey (name + SSA version) for correct identity
    std::unordered_map<VarKey, Expression*, VarKeyHash> substitutions;
    // Map from VarKey to the defining instruction (for deferred dead-marking)
    std::unordered_map<VarKey, Instruction*, VarKeyHash> def_instructions;

    // Pass 1: Build identity groups from Assignments (not Phis)
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            // Skip phi nodes
            if (isa<Phi>(inst)) continue;

            if (auto* assign = dyn_cast<Assignment>(inst)) {
                if (auto* target = dyn_cast<Variable>(assign->destination())) {
                    if (is_unstable_version_zero(target)) {
                        continue;
                    }
                    Expression* value = assign->value();
                    
                    // If assigning to a Constant or another Variable without side effects
                    if (isa<Constant>(value) || isa<Variable>(value)) {
                        auto key = var_key(target);
                        substitutions[key] = value;
                        def_instructions[key] = inst;
                    }
                }
            }
        }
    }

    // Resolve chains (e.g. A = B, B = C -> A = C). Cyclic identity
    // assignments are retained at the first repeated key rather than causing
    // an unbounded fixed-point loop.
    std::unordered_set<VarKey, VarKeyHash> cyclic_chain_keys;
    for (auto& [target_key, value] : substitutions) {
        std::unordered_set<VarKey, VarKeyHash> visited{target_key};
        while (auto* v_val = dyn_cast<Variable>(value)) {
            if (is_unstable_version_zero(v_val)) {
                break;
            }
            const auto chain_key = var_key(v_val);
            if (!visited.insert(chain_key).second) {
                cyclic_chain_keys.insert(visited.begin(), visited.end());
                break;
            }
            auto chain_it = substitutions.find(chain_key);
            if (chain_it == substitutions.end()) {
                break;
            }
            value = chain_it->second;
        }
    }
    for (const VarKey& key : cyclic_chain_keys) {
        substitutions.erase(key);
        def_instructions.erase(key);
    }

    // Pass 1.5: Verify that each substitution key has at least one use in
    // the CFG. If not, the definition is either the only reference or
    // SSA versioning has desynchronized it from its uses — do NOT mark it
    // dead, as deleting it would orphan data flow.
    std::unordered_set<VarKey, VarKeyHash> keys_with_uses;
    std::unordered_set<VarKey, VarKeyHash> keys_used_by_phi;
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            // Skip the defining instruction itself
            for (Variable* req : inst->requirements()) {
                auto key = var_key(req);
                if (substitutions.contains(key)) {
                    if (isa<Phi>(inst)) {
                        keys_used_by_phi.insert(key);
                        continue;
                    }
                    // Check this isn't the defining instruction
                    auto def_it = def_instructions.find(key);
                    if (def_it == def_instructions.end() || def_it->second != inst) {
                        keys_with_uses.insert(key);
                    }
                }
            }
        }
    }

    // Build the dead instructions set: only instructions whose definitions
    // have verified uses in the CFG.
    std::unordered_set<Instruction*> dead_instructions;
    for (auto& [key, inst] : def_instructions) {
        if (keys_with_uses.contains(key) && !keys_used_by_phi.contains(key)) {
            dead_instructions.insert(inst);
        }
    }

    // Pass 2: Apply substitutions globally and wipe dead code
    for (BasicBlock* block : task.cfg()->blocks()) {
        std::vector<Instruction*> new_insts;
        for (Instruction* inst : block->instructions()) {
            if (dead_instructions.contains(inst)) {
                continue;
            }

            // Skip phi nodes (don't substitute naively into phis)
            if (isa<Phi>(inst)) {
                new_insts.push_back(inst);
                continue;
            }

            if (auto* assign = dyn_cast<Assignment>(inst)) {
                // A non-variable destination (for example *(ptr + off)) has
                // address requirements. Those uses participate in dead-mark
                // decisions above and must be rewritten before their defining
                // identity assignments are removed.
                if (assign->destination() && !isa<Variable>(assign->destination())) {
                    substitute_uses(assign->destination(), substitutions, task.arena());
                }

                // Substitute in the value (RHS).
                Expression* value = assign->value();
                if (auto* v = dyn_cast<Variable>(value)) {
                    if (is_unstable_version_zero(v)) {
                        new_insts.push_back(inst);
                        continue;
                    }
                    auto it = substitutions.find(var_key(v));
                    if (it != substitutions.end()) {
                        assign->set_value(copy_for_use(it->second, v, task.arena()));
                    }
                } else {
                    substitute_uses(value, substitutions, task.arena());
                }
            } else if (auto* branch = dyn_cast<Branch>(inst)) {
                // Substitute in branch condition
                substitute_uses(branch->condition(), substitutions, task.arena());
            } else if (auto* ret = dyn_cast<Return>(inst)) {
                // Return values: rewrite direct variable roots and nested uses.
                for (Expression*& val : ret->mutable_values()) {
                    if (auto* v = dyn_cast<Variable>(val)) {
                        auto it = substitutions.find(var_key(v));
                        if (it != substitutions.end()) {
                            val = copy_for_use(it->second, v, task.arena());
                            continue;
                        }
                    }
                    substitute_uses(val, substitutions, task.arena());
                }
            }
            
            new_insts.push_back(inst);
        }
        block->set_instructions(std::move(new_insts));
    }
}

} // namespace aletheia
