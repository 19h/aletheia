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

static void substitute_uses(Expression* expr, const std::unordered_map<VarKey, Expression*, VarKeyHash>& subs) {
    if (!expr) return;
    if (auto* op = dyn_cast<Operation>(expr)) {
        for (size_t i = 0; i < op->operands().size(); ++i) {
            if (auto* v = dyn_cast<Variable>(op->operands()[i])) {
                auto it = subs.find(var_key(v));
                if (it != subs.end()) {
                    op->mutable_operands()[i] = it->second;
                }
            } else {
                substitute_uses(op->operands()[i], subs);
            }
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (size_t i = 0; i < list->operands().size(); ++i) {
            if (auto* v = dyn_cast<Variable>(list->operands()[i])) {
                auto it = subs.find(var_key(v));
                if (it != subs.end()) {
                    list->mutable_operands()[i] = it->second;
                }
            } else {
                substitute_uses(list->operands()[i], subs);
            }
        }
    }
}

void GraphExpressionFoldingStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

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

    // Resolve chains (e.g. A = B, B = C -> A = C)
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [target_key, value] : substitutions) {
            if (auto* v_val = dyn_cast<Variable>(value)) {
                auto chain_key = var_key(v_val);
                auto chain_it = substitutions.find(chain_key);
                if (chain_it != substitutions.end()) {
                    value = chain_it->second;
                    changed = true;
                }
            }
        }
    }

    // Pass 1.5: Verify that each substitution key has at least one use in
    // the CFG. If not, the definition is either the only reference or
    // SSA versioning has desynchronized it from its uses — do NOT mark it
    // dead, as deleting it would orphan data flow.
    std::unordered_set<VarKey, VarKeyHash> keys_with_uses;
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            // Skip the defining instruction itself
            for (Variable* req : inst->requirements()) {
                auto key = var_key(req);
                if (substitutions.contains(key)) {
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
        if (keys_with_uses.contains(key)) {
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
                // Substitute in the value (RHS) only
                Expression* value = assign->value();
                if (auto* v = dyn_cast<Variable>(value)) {
                    auto it = substitutions.find(var_key(v));
                    if (it != substitutions.end()) {
                        assign->set_value(it->second);
                    }
                } else {
                    substitute_uses(value, substitutions);
                }
            } else if (auto* branch = dyn_cast<Branch>(inst)) {
                // Substitute in branch condition
                substitute_uses(branch->condition(), substitutions);
            } else if (auto* ret = dyn_cast<Return>(inst)) {
                // Return values: substitute handled via expression walk
                for (auto* val : ret->values()) {
                    substitute_uses(val, substitutions);
                }
            }
            
            new_insts.push_back(inst);
        }
        block->set_instructions(std::move(new_insts));
    }
}

} // namespace aletheia
