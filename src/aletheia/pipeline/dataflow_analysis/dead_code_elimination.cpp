#include "dead_code_elimination.hpp"
#include <cctype>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

namespace aletheia {

static std::string to_lower_copy(const std::string& in) {
    std::string out = in;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

static std::string arm64_xw_alias_name(const std::string& name) {
    const std::string lowered = to_lower_copy(name);
    if (lowered.size() < 2) {
        return {};
    }
    const char prefix = lowered[0];
    if (prefix != 'x' && prefix != 'w') {
        return {};
    }
    for (std::size_t i = 1; i < lowered.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(lowered[i]))) {
            return {};
        }
    }

    unsigned index = 0;
    for (std::size_t i = 1; i < lowered.size(); ++i) {
        index = index * 10u + static_cast<unsigned>(lowered[i] - '0');
    }
    // x0-w7 are ABI argument/return registers and participate in many
    // artificial cross-call chains; avoid alias expansion for them.
    if (index < 8u) {
        return {};
    }

    std::string alias = lowered;
    alias[0] = (prefix == 'x') ? 'w' : 'x';
    return alias;
}

static void add_use_with_width_alias(Variable* v, std::unordered_set<std::string>& uses) {
    if (!v) {
        return;
    }

    const std::string version = std::to_string(v->ssa_version());
    uses.insert(v->name() + "_" + version);

    const std::string alias_name = arm64_xw_alias_name(v->name());
    if (!alias_name.empty()) {
        uses.insert(alias_name + "_" + version);
    }
}

static void extract_uses(Expression* expr, std::unordered_set<std::string>& uses) {
    if (!expr) return;
    if (auto* v = dyn_cast<Variable>(expr)) {
        add_use_with_width_alias(v, uses);
    } else if (auto* op = dyn_cast<Operation>(expr)) {
        for (Expression* child : op->operands()) {
            extract_uses(child, uses);
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            extract_uses(child, uses);
        }
    }
}

static std::string var_key_string(Variable* v) {
    if (!v) return {};
    return v->name() + "_" + std::to_string(v->ssa_version());
}


// recursively check for calls
static bool contains_call(Expression* expr) {
    if (!expr) return false;
    if (isa<Call>(expr)) return true;
    if (auto* op = dyn_cast<Operation>(expr)) {
        if (op->type() == OperationType::call) return true;
        for (Expression* child : op->operands()) {
            if (contains_call(child)) return true;
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            if (contains_call(child)) return true;
        }
    }
    return false;
}




static bool assignment_has_side_effect_destination(Assignment* assign) {
    if (!assign) return false;
    Expression* dest = assign->destination();
    if (isa<Variable>(dest)) return false;
    // dereference assignments have side effects
    if (auto* op = dyn_cast<Operation>(dest)) {
        if (op->type() == OperationType::deref) return true;
    }
    return true;
}

void DeadCodeEliminationStage::execute(DecompilerTask& task) {



    if (!task.cfg()) return;

    bool changed = true;
    while (changed) {
        changed = false;
        
        std::unordered_set<std::string> global_uses;
        std::unordered_map<std::string, Assignment*> def_of;

        // Track direct sink requirements (branch conditions + return values)
        // and preserve the whole def-use chain feeding those sinks.
        std::unordered_set<std::string> sink_roots;

        // Pass 1: Collect all variable uses across the CFG
        for (BasicBlock* bb : task.cfg()->blocks()) {
            for (Instruction* inst : bb->instructions()) {
                if (auto* assign = dyn_cast<Assignment>(inst)) {
                    if (auto* dst = dyn_cast<Variable>(assign->destination())) {
                        def_of[var_key_string(dst)] = assign;
                    }
                    // Uses from the RHS
                    extract_uses(assign->value(), global_uses);
                    // If destination is a complex expression (e.g., deref), its
                    // sub-expressions are also uses
                    if (!isa<Variable>(assign->destination())) {
                        extract_uses(assign->destination(), global_uses);
                    }
                } else if (auto* branch = dyn_cast<Branch>(inst)) {
                    extract_uses(branch->condition(), global_uses);
                    std::unordered_set<Variable*> reqs;
                    branch->collect_requirements(reqs);
                    for (Variable* req : reqs) {
                        sink_roots.insert(var_key_string(req));
                    }
                } else if (auto* ret = dyn_cast<Return>(inst)) {
                    for (auto* val : ret->values()) {
                        extract_uses(val, global_uses);
                    }
                    std::unordered_set<Variable*> reqs;
                    ret->collect_requirements(reqs);
                    for (Variable* req : reqs) {
                        sink_roots.insert(var_key_string(req));
                    }
                } else if (auto* phi = dyn_cast<Phi>(inst)) {
                    // Phi operands are uses
                    if (phi->operand_list()) {
                        extract_uses(phi->operand_list(), global_uses);
                    }
                }
                // Break, Continue, Comment have no variable references
            }
        }

        std::unordered_set<std::string> protected_defs;
        std::vector<std::string> stack(sink_roots.begin(), sink_roots.end());
        while (!stack.empty()) {
            const std::string current = stack.back();
            stack.pop_back();
            if (current.empty() || protected_defs.contains(current)) {
                continue;
            }
            protected_defs.insert(current);

            auto it = def_of.find(current);
            if (it == def_of.end() || !it->second) {
                continue;
            }

            std::unordered_set<Variable*> rhs_reqs;
            it->second->collect_requirements(rhs_reqs);
            for (Variable* req : rhs_reqs) {
                const std::string dep = var_key_string(req);
                if (!dep.empty() && !protected_defs.contains(dep)) {
                    stack.push_back(dep);
                }
            }
        }

        // Pass 2: Remove assignments whose definitions are not used
        for (BasicBlock* bb : task.cfg()->blocks()) {
            std::vector<Instruction*> new_insts;
            for (Instruction* inst : bb->instructions()) {
                if (auto* assign = dyn_cast<Assignment>(inst)) {
                    // Don't eliminate phis in this pass
                    if (isa<Phi>(inst)) {
                        new_insts.push_back(inst);
                        continue;
                    }
                    
                    if (auto* target = dyn_cast<Variable>(assign->destination())) {
                        std::string def_name = target->name() + "_" + std::to_string(target->ssa_version());

                        if (protected_defs.contains(def_name)) {
                            new_insts.push_back(inst);
                            continue;
                        }
                        
                        const std::string lowered_name = to_lower_copy(target->name());

                        // Don't eliminate definitions of special registers (return values, stack pointer)
                        if (!global_uses.contains(def_name) && 
                            lowered_name != "sp" && lowered_name != "w0" && lowered_name != "x0") {
                            
                            // DO NOT ELIMINATE FUNCTION CALLS
                            if (!contains_call(assign->value()) && !assignment_has_side_effect_destination(assign)) {
                                changed = true;
                                continue; // Drop instruction
                            }
                        }
                    }
                }
                new_insts.push_back(inst);
            }
            if (new_insts.size() != bb->instructions().size()) {
                bb->set_instructions(std::move(new_insts));
            }
        }
    }
}

} // namespace aletheia
