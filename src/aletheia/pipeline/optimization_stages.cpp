#include "optimization_stages.hpp"
#include "../../logos/z3_logic.hpp"
#include "../ssa/dominators.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace aletheia {

// =============================================================================
// Variable identity key for SSA-form variables
// =============================================================================
// Variables in SSA are uniquely identified by (name, ssa_version).

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

// =============================================================================
// Expression tree operations: contains_variable, replace_variable
// =============================================================================

static bool contains_variable_ptr(Expression* expr, Variable* target) {
    if (!expr) return false;

    if (auto* v = dynamic_cast<Variable*>(expr)) {
        return v->name() == target->name() &&
               v->ssa_version() == target->ssa_version();
    }

    if (auto* op = dynamic_cast<Operation*>(expr)) {
        for (auto* child : op->operands()) {
            if (contains_variable_ptr(child, target)) return true;
        }
    }

    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (auto* child : list->operands()) {
            if (contains_variable_ptr(child, target)) return true;
        }
    }

    return false;
}

static Expression* replace_variable_ptr(
    DecompilerArena& arena, Expression* expr,
    Variable* target, Expression* replacement) {
    if (!expr) return nullptr;

    if (auto* v = dynamic_cast<Variable*>(expr)) {
        if (v->name() == target->name() &&
            v->ssa_version() == target->ssa_version()) {
            return replacement;
        }
        return v;
    }

    if (auto* c = dynamic_cast<Constant*>(expr)) {
        return c;
    }

    if (auto* cond = dynamic_cast<Condition*>(expr)) {
        Expression* new_lhs = replace_variable_ptr(arena, cond->lhs(), target, replacement);
        Expression* new_rhs = replace_variable_ptr(arena, cond->rhs(), target, replacement);
        if (new_lhs != cond->lhs() || new_rhs != cond->rhs()) {
            return arena.create<Condition>(cond->type(), new_lhs, new_rhs, cond->size_bytes);
        }
        return cond;
    }

    if (auto* op = dynamic_cast<Operation*>(expr)) {
        std::vector<Expression*> new_operands;
        bool changed = false;
        for (auto* child : op->operands()) {
            Expression* new_child = replace_variable_ptr(arena, child, target, replacement);
            if (new_child != child) changed = true;
            new_operands.push_back(new_child);
        }
        if (changed) {
            return arena.create<Operation>(op->type(), std::move(new_operands), op->size_bytes);
        }
        return op;
    }

    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        std::vector<Expression*> new_operands;
        bool changed = false;
        for (auto* child : list->operands()) {
            Expression* new_child = replace_variable_ptr(arena, child, target, replacement);
            if (new_child != child) changed = true;
            new_operands.push_back(new_child);
        }
        if (changed) {
            return arena.create<ListOperation>(std::move(new_operands), list->size_bytes);
        }
        return list;
    }

    return expr;
}

// =============================================================================
// Rule checks for safe propagation
// =============================================================================

/// Check if an expression contains any aliased variable.
static bool contains_aliased_variable(Expression* expr) {
    if (!expr) return false;
    if (auto* v = dynamic_cast<Variable*>(expr)) return v->is_aliased();
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        for (auto* child : op->operands())
            if (contains_aliased_variable(child)) return true;
    }
    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (auto* child : list->operands())
            if (contains_aliased_variable(child)) return true;
    }
    return false;
}

/// Check if an expression is an address_of operation.
static bool is_address_of(Expression* expr) {
    if (auto* op = dynamic_cast<Operation*>(expr))
        return op->type() == OperationType::address_of;
    return false;
}

/// Check if an expression contains a dereference operation.
static bool contains_dereference(Expression* expr) {
    if (!expr) return false;
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        if (op->type() == OperationType::deref) return true;
        for (auto* child : op->operands())
            if (contains_dereference(child)) return true;
    }
    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (auto* child : list->operands())
            if (contains_dereference(child)) return true;
    }
    return false;
}

/// Check if a definition (Assignment) can be safely propagated into a target
/// instruction. Returns true if the propagation is allowed.
static bool definition_can_be_propagated(Assignment* def, Instruction* target) {
    // Rule: Don't propagate phi definitions
    if (dynamic_cast<Phi*>(def)) return false;

    // Rule: Don't propagate call assignments (handled by EPFC)
    if (auto* val = dynamic_cast<Operation*>(def->value())) {
        if (val->type() == OperationType::call) return false;
    }

    // Rule: Don't propagate if definition contains aliased variables (EPM handles)
    if (contains_aliased_variable(def->destination()) ||
        contains_aliased_variable(def->value())) return false;

    // Rule: Don't propagate address_of assignments
    if (is_address_of(def->value())) return false;

    // Rule: Don't propagate if the definition's RHS contains dereferences (EPM handles)
    if (contains_dereference(def->value())) return false;

    // Rule: Don't propagate operations into phi arguments
    if (dynamic_cast<Phi*>(target)) {
        if (dynamic_cast<Operation*>(def->value())) return false;
    }

    return true;
}

// =============================================================================
// CMP+Branch folding helper
// =============================================================================
// If flags = a - b and we have if (flags CMP 0), fold to if (a CMP b).

static void try_fold_cmp_branch(DecompilerArena& arena, Branch* branch) {
    Condition* cond = branch->condition();
    Expression* lhs = cond->lhs();
    Expression* rhs = cond->rhs();

    if (auto* sub_op = dynamic_cast<Operation*>(lhs)) {
        if (sub_op->type() == OperationType::sub && sub_op->operands().size() == 2) {
            if (auto* zero = dynamic_cast<Constant*>(rhs)) {
                if (zero->value() == 0) {
                    auto* new_cond = arena.create<Condition>(
                        cond->type(), sub_op->operands()[0], sub_op->operands()[1], cond->size_bytes);
                    branch->set_condition(new_cond);
                }
            }
        }
    }
}

// =============================================================================
// Redundant phi removal
// =============================================================================
// A phi is redundant if all its RHS operands (ignoring self-references) are
// the same variable. In that case, replace all uses of the phi's destination
// with that single value.

static void remove_redundant_phis(ControlFlowGraph* cfg, DecompilerArena& arena) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (BasicBlock* block : cfg->blocks()) {
            auto& instrs = block->mutable_instructions();
            for (std::size_t i = 0; i < instrs.size(); ) {
                auto* phi = dynamic_cast<Phi*>(instrs[i]);
                if (!phi) { ++i; continue; }

                Variable* dest = phi->dest_var();
                Expression* single_value = nullptr;
                bool all_same = true;

                for (auto* operand : phi->operand_list()->operands()) {
                    // Skip self-references (phi dest used in its own operand list)
                    if (auto* v = dynamic_cast<Variable*>(operand)) {
                        if (v->name() == dest->name() &&
                            v->ssa_version() == dest->ssa_version()) continue;
                    }
                    if (!single_value) {
                        single_value = operand;
                    } else {
                        // Check if this operand is the same as single_value
                        auto* sv = dynamic_cast<Variable*>(single_value);
                        auto* ov = dynamic_cast<Variable*>(operand);
                        if (sv && ov && sv->name() == ov->name() &&
                            sv->ssa_version() == ov->ssa_version()) {
                            // Same variable, continue
                        } else {
                            all_same = false;
                            break;
                        }
                    }
                }

                if (all_same && single_value) {
                    // This phi is redundant: dest = single_value
                    // Replace with a simple assignment
                    auto* assign = arena.create<Assignment>(dest, single_value);
                    assign->set_address(phi->address());
                    instrs[i] = assign;
                    changed = true;
                    ++i;
                } else {
                    ++i;
                }
            }
        }
    }
}

// =============================================================================
// IdentityElimination helpers
// =============================================================================

namespace {

struct DisjointSet {
    std::vector<int> parent;
    std::vector<int> rank;

    explicit DisjointSet(std::size_t n) : parent(n), rank(n, 0) {
        for (std::size_t i = 0; i < n; ++i) parent[i] = static_cast<int>(i);
    }

    int find(int x) {
        if (parent[x] != x) parent[x] = find(parent[x]);
        return parent[x];
    }

    bool unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        if (rank[a] < rank[b]) std::swap(a, b);
        parent[b] = a;
        if (rank[a] == rank[b]) ++rank[a];
        return true;
    }
};

struct PhiIdentityInfo {
    VarKey dest;
    std::vector<VarKey> operands;
};

static bool type_compatible_for_identity(Variable* a, Variable* b) {
    if (!a || !b) return false;

    if (a->is_aliased() != b->is_aliased()) return false;
    if (a->is_aliased() && b->is_aliased() && a->name() != b->name()) return false;

    auto ta = a->ir_type();
    auto tb = b->ir_type();
    if (ta && tb) {
        return ta->to_string() == tb->to_string();
    }

    return a->size_bytes == b->size_bytes;
}

static Variable* representative_for_identity_component(
    const std::vector<VarKey>& keys,
    const std::unordered_map<VarKey, Variable*, VarKeyHash>& sample_of) {
    if (keys.empty()) return nullptr;

    const VarKey* chosen = &keys[0];
    for (const VarKey& key : keys) {
        if (key.ssa_version < chosen->ssa_version) {
            chosen = &key;
            continue;
        }
        if (key.ssa_version == chosen->ssa_version && key.name < chosen->name) {
            chosen = &key;
        }
    }

    auto it = sample_of.find(*chosen);
    return (it != sample_of.end()) ? it->second : nullptr;
}

} // namespace

// =============================================================================
// Dangerous Memory Check Helpers
// =============================================================================

static bool is_dangerous_memory_instruction(Instruction* inst) {
    if (dynamic_cast<Relation*>(inst)) return true;
    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
        if (contains_dereference(assign->destination())) return true;
        if (auto* op = dynamic_cast<Operation*>(assign->value())) {
            if (op->type() == OperationType::call) return true;
        }
        if (dynamic_cast<Call*>(assign->value())) return true;
    }
    return false;
}

struct InstLocation {
    BasicBlock* block;
    std::size_t index;
};

// =============================================================================
// ExpressionPropagation Common Logic
// =============================================================================

static void expression_propagation_impl(DecompilerTask& task, bool is_memory) {
    if (!task.cfg()) return;

    // Cache reachability matrix if memory mode
    std::unordered_map<BasicBlock*, std::unordered_set<BasicBlock*>> reachability;
    if (is_memory) {
        for (BasicBlock* b : task.cfg()->blocks()) {
            std::vector<BasicBlock*> stack = {b};
            while (!stack.empty()) {
                BasicBlock* curr = stack.back();
                stack.pop_back();
                for (Edge* e : curr->successors()) {
                    if (reachability[b].insert(e->target()).second) {
                        stack.push_back(e->target());
                    }
                }
            }
        }
    }

    auto has_path = [&](BasicBlock* source, BasicBlock* sink) {
        if (source == sink) return true;
        return reachability[source].contains(sink);
    };

    // Fixed-point outer loop
    for (int iteration = 0; iteration < 100; ++iteration) {
        // Step 1: Remove redundant phis before each iteration
        remove_redundant_phis(task.cfg(), task.arena());

        std::unordered_map<Instruction*, InstLocation> loc_map;
        std::vector<Instruction*> dangerous_uses;

        // Step 2: Build DefMap globally (Variable -> Assignment*)
        using DefMap = std::unordered_map<VarKey, Assignment*, VarKeyHash>;
        DefMap def_map;

        for (BasicBlock* block : task.cfg()->blocks()) {
            for (std::size_t i = 0; i < block->instructions().size(); ++i) {
                Instruction* inst = block->instructions()[i];
                loc_map[inst] = {block, i};
                if (is_memory && is_dangerous_memory_instruction(inst)) {
                    dangerous_uses.push_back(inst);
                }

                auto* assign = dynamic_cast<Assignment*>(inst);
                if (!assign) continue;
                if (auto* dest = dynamic_cast<Variable*>(assign->destination())) {
                    VarKey key = var_key(dest);
                    if (!def_map.contains(key)) {
                        def_map[key] = assign;
                    }
                }
            }
        }

        auto has_any_dangerous_use = [&](Instruction* def, Instruction* target) {
            auto def_loc = loc_map[def];
            auto target_loc = loc_map[target];

            for (Instruction* use : dangerous_uses) {
                auto use_loc = loc_map[use];
                if (def_loc.block == target_loc.block) {
                    if (use_loc.block == def_loc.block && use_loc.index > def_loc.index && use_loc.index < target_loc.index) {
                        return true;
                    }
                } else {
                    if (use_loc.block == target_loc.block) {
                        if (use_loc.index < target_loc.index && has_path(def_loc.block, use_loc.block)) return true;
                    } else if (use_loc.block == def_loc.block) {
                        if (use_loc.index > def_loc.index && has_path(use_loc.block, target_loc.block)) return true;
                    } else {
                        if (has_path(def_loc.block, use_loc.block) && has_path(use_loc.block, target_loc.block)) {
                            return true;
                        }
                    }
                }
            }
            return false;
        };

        // Step 3: Iterate blocks in RPO and propagate definitions
        bool any_change = false;

        for (BasicBlock* block : task.cfg()->reverse_post_order()) {
            auto& instrs = block->mutable_instructions();

            for (std::size_t idx = 0; idx < instrs.size(); ++idx) {
                Instruction* inst = instrs[idx];

                // Collect the variables required by this instruction
                std::unordered_set<Variable*> reqs;
                inst->collect_requirements(reqs);

                for (Variable* req_var : reqs) {
                    VarKey key = var_key(req_var);
                    auto it = def_map.find(key);
                    if (it == def_map.end()) continue;

                    Assignment* def = it->second;

                    // Don't propagate a definition into itself
                    if (static_cast<Instruction*>(def) == inst) continue;

                    // Apply rule checks
                    bool allowed = true;
                    if (dynamic_cast<Phi*>(def)) allowed = false;
                    else if (auto* val = dynamic_cast<Operation*>(def->value())) {
                        if (val->type() == OperationType::call) allowed = false;
                    } else if (dynamic_cast<Call*>(def->value())) {
                        allowed = false;
                    }
                    if (allowed && is_address_of(def->value())) allowed = false;
                    if (allowed && dynamic_cast<Phi*>(inst) && dynamic_cast<Operation*>(def->value())) allowed = false;

                    if (!allowed) continue;

                    if (!is_memory) {
                        if (contains_aliased_variable(def->destination()) ||
                            contains_aliased_variable(def->value())) continue;
                        if (contains_dereference(def->value())) continue;
                    } else {
                        // Memory mode specific rules:
                        // We ONLY want to path-check if it actually contains an aliased var or deref
                        // Actually, if it DOESN'T contain aliased/deref, we don't need the dangerous use check
                        if (contains_aliased_variable(def->destination()) ||
                            contains_aliased_variable(def->value()) ||
                            contains_dereference(def->value())) {

                            if (has_any_dangerous_use(def, inst)) continue;
                        }
                    }

                    Expression* replacement = def->value();
                    if (!replacement) continue;

                    // Perform the substitution on the instruction
                    bool changed = false;

                    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                        Expression* new_val = replace_variable_ptr(
                            task.arena(), assign->value(), req_var, replacement);
                        if (new_val != assign->value()) {
                            assign->set_value(new_val);
                            changed = true;
                        }
                        // Also propagate into complex destinations (e.g., *(ptr+off) = val)
                        if (!dynamic_cast<Variable*>(assign->destination())) {
                            Expression* new_dest = replace_variable_ptr(
                                task.arena(), assign->destination(), req_var, replacement);
                            if (new_dest != assign->destination()) {
                                assign->set_destination(new_dest);
                                changed = true;
                            }
                        }
                    } else if (auto* branch = dynamic_cast<Branch*>(inst)) {
                        Condition* cond = branch->condition();
                        Expression* new_lhs = replace_variable_ptr(
                            task.arena(), cond->lhs(), req_var, replacement);
                        Expression* new_rhs = replace_variable_ptr(
                            task.arena(), cond->rhs(), req_var, replacement);
                        if (new_lhs != cond->lhs() || new_rhs != cond->rhs()) {
                            auto* new_cond = task.arena().create<Condition>(
                                cond->type(), new_lhs, new_rhs, cond->size_bytes);
                            branch->set_condition(new_cond);
                            changed = true;
                        }
                    } else if (auto* ret = dynamic_cast<Return*>(inst)) {
                        // Return stores values directly; we need to rebuild
                        std::vector<Expression*> new_vals;
                        bool ret_changed = false;
                        for (auto* val : ret->values()) {
                            Expression* new_val = replace_variable_ptr(
                                task.arena(), val, req_var, replacement);
                            if (new_val != val) ret_changed = true;
                            new_vals.push_back(new_val);
                        }
                        if (ret_changed) {
                            auto* new_ret = task.arena().create<Return>(std::move(new_vals));
                            new_ret->set_address(ret->address());
                            instrs[idx] = new_ret;
                            inst = new_ret;
                            changed = true;
                        }
                    }

                    if (changed) any_change = true;
                }

                // CMP+branch folding after propagation
                if (auto* branch = dynamic_cast<Branch*>(inst)) {
                    try_fold_cmp_branch(task.arena(), branch);
                }
            }
        }

        // Fixed-point: stop if no changes occurred in this iteration
        if (!any_change) break;
    }
}

// =============================================================================
// ExpressionPropagationStage
// =============================================================================

void ExpressionPropagationStage::execute(DecompilerTask& task) {
    expression_propagation_impl(task, false);
}

void ExpressionPropagationMemoryStage::execute(DecompilerTask& task) {
    expression_propagation_impl(task, true);
}

void DeadPathEliminationStage::execute(DecompilerTask& task) {
    if (!task.cfg() || !task.cfg()->entry_block()) return;

    z3::context& ctx = task.z3_ctx();
    logos::Z3Converter converter(ctx);

    std::unordered_set<Edge*> dead_edges;

    for (BasicBlock* block : task.cfg()->blocks()) {
        if (block->successors().size() > 1) {
            if (block->instructions().empty()) continue;
            Instruction* last_inst = block->instructions().back();
            if (auto* branch = dynamic_cast<Branch*>(last_inst)) {
                // If the branch is a generic Branch (not indirect), check its condition
                logos::LogicCondition cond = converter.convert_to_condition(branch->condition());

                for (Edge* edge : block->successors()) {
                    bool invalid = false;
                    if (edge->type() == EdgeType::False) {
                        invalid = cond.negate().is_not_satisfiable();
                    } else if (edge->type() == EdgeType::True) {
                        invalid = cond.is_not_satisfiable();
                    }
                    if (invalid) {
                        dead_edges.insert(edge);
                    }
                }
            }
        }
    }

    if (dead_edges.empty()) return;

    for (Edge* dead_edge : dead_edges) {
        BasicBlock* source = dead_edge->source();

        // Remove the branch instruction if it's there
        if (!source->instructions().empty()) {
            if (dynamic_cast<Branch*>(source->instructions().back())) {
                source->mutable_instructions().pop_back();
            }
        }

        // Remove the dead edge from the graph
        task.cfg()->remove_edge(dead_edge);

        // Turn all remaining out-edges into unconditional
        std::vector<Edge*> remaining_edges = source->successors();
        for (Edge* remaining_edge : remaining_edges) {
            Edge* unconditional = task.arena().create<Edge>(remaining_edge->source(), remaining_edge->target(), EdgeType::Unconditional);
            task.cfg()->substitute_edge(remaining_edge, unconditional);
        }
    }

    // Find unreachable blocks
    std::vector<BasicBlock*> reachable_list = task.cfg()->post_order();
    std::unordered_set<BasicBlock*> reachable(reachable_list.begin(), reachable_list.end());

    std::unordered_set<BasicBlock*> dead_blocks;
    for (BasicBlock* block : task.cfg()->blocks()) {
        if (!reachable.contains(block)) {
            dead_blocks.insert(block);
        }
    }

    // Fix phi origin blocks
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* phi = dynamic_cast<Phi*>(inst)) {
                std::vector<BasicBlock*> to_remove;
                for (auto& pair : phi->origin_block()) {
                    if (dead_blocks.contains(pair.first)) {
                        to_remove.push_back(pair.first);
                    }
                }
                for (BasicBlock* dead_pred : to_remove) {
                    phi->remove_from_origin_block(dead_pred);
                }
            }
        }
    }

    if (!dead_blocks.empty()) {
        task.cfg()->remove_nodes_from(dead_blocks);
    }
}

void DeadLoopEliminationStage::execute(DecompilerTask& task) {
    if (!task.cfg() || !task.cfg()->entry_block()) return;

    z3::context& ctx = task.z3_ctx();
    logos::Z3Converter converter(ctx);

    using DefMap = std::unordered_map<VarKey, Assignment*, VarKeyHash>;
    DefMap def_map;
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                if (auto* dest = dynamic_cast<Variable*>(assign->destination())) {
                    VarKey key = var_key(dest);
                    if (!def_map.contains(key)) {
                        def_map[key] = assign;
                    }
                }
            }
        }
    }

    std::unordered_set<Edge*> prunable_edges;

    for (BasicBlock* block : task.cfg()->blocks()) {
        if (block->successors().size() > 1) {
            if (block->instructions().empty()) continue;
            Instruction* last_inst = block->instructions().back();
            if (auto* branch = dynamic_cast<Branch*>(last_inst)) {
                // Dependency dict: variables required by branch -> Phi that defines them
                std::unordered_map<Variable*, Phi*> phi_dependencies;
                std::unordered_set<Variable*> reqs;
                branch->collect_requirements(reqs);

                for (Variable* req : reqs) {
                    if (req->is_aliased()) continue;
                    auto it = def_map.find(var_key(req));
                    if (it != def_map.end()) {
                        if (auto* phi = dynamic_cast<Phi*>(it->second)) {
                            phi_dependencies[req] = phi;
                        }
                    }
                }

                if (phi_dependencies.empty()) continue;

                // Resolve phi values
                std::unordered_map<Variable*, Constant*> substituted_constants;
                for (auto& [req, phi] : phi_dependencies) {
                    Constant* unique_upstream_value = nullptr;
                    bool multiple_values = false;
                    for (auto& [var_block, expr] : phi->origin_block()) {
                        if (auto* c = dynamic_cast<Constant*>(expr)) {
                            // If var_block == block, or no path exists from block to var_block
                            // (we skip precise dominator checks here for simplicity and assume it's valid if it reaches)
                            if (!unique_upstream_value) {
                                unique_upstream_value = c;
                            } else if (unique_upstream_value->value() != c->value()) {
                                multiple_values = true;
                                break;
                            }
                        } else {
                            multiple_values = true;
                            break;
                        }
                    }

                    if (unique_upstream_value && !multiple_values) {
                        substituted_constants[req] = unique_upstream_value;
                    }
                }

                if (substituted_constants.empty()) continue;

                // Patch condition
                Expression* patched_condition_expr = branch->condition()->copy(task.arena());
                for (auto& [var, constant] : substituted_constants) {
                    patched_condition_expr = replace_variable_ptr(task.arena(), patched_condition_expr, var, constant);
                }

                if (auto* patched_cond = dynamic_cast<Condition*>(patched_condition_expr)) {
                    logos::LogicCondition cond = converter.convert_to_condition(patched_cond);

                    Edge* sat_edge = nullptr;
                    Edge* unsat_edge = nullptr;

                    for (Edge* edge : block->successors()) {
                        bool invalid = false;
                        if (edge->type() == EdgeType::False) {
                            invalid = cond.negate().is_not_satisfiable();
                        } else if (edge->type() == EdgeType::True) {
                            invalid = cond.is_not_satisfiable();
                        }

                        if (invalid) {
                            if (!unsat_edge) unsat_edge = edge;
                        } else {
                            sat_edge = edge;
                        }
                    }

                    if (unsat_edge && sat_edge) {
                        // Check reachability: can we reach block from sat_edge->target()?
                        // If not, we never come back, meaning unsat_edge is prunable.
                        // Simple DFS check:
                        std::unordered_set<BasicBlock*> visited;
                        std::vector<BasicBlock*> stack = { sat_edge->target() };
                        bool reachable = false;
                        while (!stack.empty()) {
                            BasicBlock* curr = stack.back();
                            stack.pop_back();
                            if (curr == block) {
                                reachable = true;
                                break;
                            }
                            if (visited.contains(curr)) continue;
                            visited.insert(curr);
                            for (Edge* out : curr->successors()) {
                                stack.push_back(out->target());
                            }
                        }

                        if (!reachable) {
                            prunable_edges.insert(unsat_edge);
                        }
                    }
                }
            }
        }
    }

    if (prunable_edges.empty()) return;

    for (Edge* dead_edge : prunable_edges) {
        BasicBlock* source = dead_edge->source();
        if (!source->instructions().empty()) {
            if (dynamic_cast<Branch*>(source->instructions().back())) {
                source->mutable_instructions().pop_back();
            }
        }
        task.cfg()->remove_edge(dead_edge);

        std::vector<Edge*> remaining_edges = source->successors();
        for (Edge* remaining_edge : remaining_edges) {
            Edge* unconditional = task.arena().create<Edge>(remaining_edge->source(), remaining_edge->target(), EdgeType::Unconditional);
            task.cfg()->substitute_edge(remaining_edge, unconditional);
        }
    }

    // Unreachable blocks / fix origin phis logic (same as dead path)
    std::vector<BasicBlock*> reachable_list = task.cfg()->post_order();
    std::unordered_set<BasicBlock*> reachable(reachable_list.begin(), reachable_list.end());

    std::unordered_set<BasicBlock*> dead_blocks;
    for (BasicBlock* block : task.cfg()->blocks()) {
        if (!reachable.contains(block)) {
            dead_blocks.insert(block);
        }
    }

    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* phi = dynamic_cast<Phi*>(inst)) {
                std::vector<BasicBlock*> to_remove;
                for (auto& pair : phi->origin_block()) {
                    if (dead_blocks.contains(pair.first)) {
                        to_remove.push_back(pair.first);
                    }
                }
                for (BasicBlock* dead_pred : to_remove) {
                    phi->remove_from_origin_block(dead_pred);
                }
            }
        }
    }

    if (!dead_blocks.empty()) {
        task.cfg()->remove_nodes_from(dead_blocks);
    }
}
void ExpressionPropagationFunctionCallStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    // Fixed-point loop for function call propagation
    bool changed = true;
    while (changed) {
        changed = false;

        using DefMap = std::unordered_map<VarKey, Assignment*, VarKeyHash>;
        DefMap def_map;

        using UseSet = std::unordered_set<Instruction*>;
        std::unordered_map<VarKey, UseSet, VarKeyHash> use_map;

        for (BasicBlock* block : task.cfg()->blocks()) {
            for (Instruction* inst : block->instructions()) {
                if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                    if (auto* dest = dynamic_cast<Variable*>(assign->destination())) {
                        VarKey key = var_key(dest);
                        if (!def_map.contains(key)) def_map[key] = assign;
                    }
                }
                std::unordered_set<Variable*> reqs;
                inst->collect_requirements(reqs);
                for (Variable* req : reqs) {
                    use_map[var_key(req)].insert(inst);
                }
            }
        }

        for (BasicBlock* block : task.cfg()->blocks()) {
            auto& instrs = block->mutable_instructions();
            for (std::size_t i = 0; i < instrs.size(); ++i) {
                Instruction* inst = instrs[i];
                std::unordered_set<Variable*> reqs;
                inst->collect_requirements(reqs);

                for (Variable* req_var : reqs) {
                    VarKey key = var_key(req_var);
                    auto it_def = def_map.find(key);
                    if (it_def == def_map.end()) continue;
                    Assignment* def = it_def->second;

                    // We only propagate call assignments
                    bool is_call = false;
                    if (auto* op = dynamic_cast<Operation*>(def->value())) {
                        if (op->type() == OperationType::call) is_call = true;
                    } else if (dynamic_cast<Call*>(def->value())) {
                        is_call = true;
                    }

                    if (!is_call) continue;

                    // It must be used exactly once
                    if (use_map[key].size() != 1) continue;

                    // Path-based memory safety (conservative):
                    // A call modifies/reads memory, so no dangerous memory instruction
                    // between the call and its single use.
                    // To keep it simple in this pass, we just verify it's the exact same block
                    // and no dangerous uses between them.
                    bool safe = true;
                    BasicBlock* use_block = nullptr;
                    std::size_t use_idx = 0;
                    for (BasicBlock* ub : task.cfg()->blocks()) {
                        for (std::size_t ui = 0; ui < ub->instructions().size(); ++ui) {
                            if (ub->instructions()[ui] == inst) {
                                use_block = ub;
                                use_idx = ui;
                                break;
                            }
                        }
                    }

                    BasicBlock* def_block = nullptr;
                    std::size_t def_idx = 0;
                    for (BasicBlock* db : task.cfg()->blocks()) {
                        for (std::size_t di = 0; db->instructions().size() > di; ++di) {
                            if (db->instructions()[di] == def) {
                                def_block = db;
                                def_idx = di;
                                break;
                            }
                        }
                    }

                    // Strict safety: same block, def_idx < use_idx, and no dangerous calls/relations in between
                    if (def_block != use_block || def_idx >= use_idx) {
                        // Complex cross-block tracking skipped for now; assume unsafe
                        safe = false;
                    } else {
                        for (std::size_t idx = def_idx + 1; idx < use_idx; ++idx) {
                            if (is_dangerous_memory_instruction(use_block->instructions()[idx])) {
                                safe = false;
                                break;
                            }
                        }
                    }

                    if (!safe) continue;

                    // Perform substitution
                    Expression* replacement = def->value();
                    bool sub_changed = false;

                    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                        Expression* new_val = replace_variable_ptr(task.arena(), assign->value(), req_var, replacement);
                        if (new_val != assign->value()) {
                            assign->set_value(new_val);
                            sub_changed = true;
                        }
                        if (!dynamic_cast<Variable*>(assign->destination())) {
                            Expression* new_dest = replace_variable_ptr(task.arena(), assign->destination(), req_var, replacement);
                            if (new_dest != assign->destination()) {
                                assign->set_destination(new_dest);
                                sub_changed = true;
                            }
                        }
                    } else if (auto* branch = dynamic_cast<Branch*>(inst)) {
                        Condition* cond = branch->condition();
                        Expression* new_lhs = replace_variable_ptr(task.arena(), cond->lhs(), req_var, replacement);
                        Expression* new_rhs = replace_variable_ptr(task.arena(), cond->rhs(), req_var, replacement);
                        if (new_lhs != cond->lhs() || new_rhs != cond->rhs()) {
                            auto* new_cond = task.arena().create<Condition>(cond->type(), new_lhs, new_rhs, cond->size_bytes);
                            branch->set_condition(new_cond);
                            sub_changed = true;
                        }
                    } else if (auto* ret = dynamic_cast<Return*>(inst)) {
                        std::vector<Expression*> new_vals;
                        for (auto* val : ret->values()) {
                            Expression* new_val = replace_variable_ptr(task.arena(), val, req_var, replacement);
                            if (new_val != val) sub_changed = true;
                            new_vals.push_back(new_val);
                        }
                        if (sub_changed) {
                            auto* new_ret = task.arena().create<Return>(std::move(new_vals));
                            new_ret->set_address(ret->address());
                            instrs[i] = new_ret;
                            inst = new_ret;
                        }
                    }

                    if (sub_changed) {
                        // Replace the original call assignment with var = 0 (Constant 0)
                        // It will be cleaned up by DeadCodeElimination if not used.
                        def->set_value(task.arena().create<Constant>(0, def->value()->size_bytes));
                        changed = true;
                        break; // restart iteration since we modified instructions list and maps
                    }
                }
                if (changed) break;
            }
            if (changed) break;
        }
    }
}

void IdentityEliminationStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    // Step 1: collect variable universe + direct identity edges + phi candidates
    std::unordered_map<VarKey, int, VarKeyHash> id_of;
    std::vector<VarKey> keys;
    std::unordered_map<VarKey, Variable*, VarKeyHash> sample_of;
    std::vector<std::pair<VarKey, VarKey>> direct_identity_edges;
    std::vector<PhiIdentityInfo> phi_infos;

    auto intern = [&](Variable* v) {
        if (!v) return;
        VarKey k = var_key(v);
        if (!id_of.contains(k)) {
            id_of[k] = static_cast<int>(keys.size());
            keys.push_back(k);
        }
        if (!sample_of.contains(k)) {
            sample_of[k] = v;
        }
    };

    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            for (Variable* req : inst->requirements()) intern(req);
            for (Variable* def : inst->definitions()) intern(def);

            auto* assign = dynamic_cast<Assignment*>(inst);
            if (!assign) continue;

            auto* dest = dynamic_cast<Variable*>(assign->destination());
            if (!dest) continue;

            if (auto* src = dynamic_cast<Variable*>(assign->value())) {
                if (type_compatible_for_identity(dest, src)) {
                    direct_identity_edges.push_back({var_key(dest), var_key(src)});
                }
                continue;
            }

            auto* phi = dynamic_cast<Phi*>(inst);
            if (!phi || !phi->operand_list()) continue;

            PhiIdentityInfo info{var_key(dest), {}};
            for (Expression* op : phi->operand_list()->operands()) {
                auto* ov = dynamic_cast<Variable*>(op);
                if (!ov) {
                    info.operands.clear();
                    break;
                }
                info.operands.push_back(var_key(ov));
            }
            if (!info.operands.empty()) {
                phi_infos.push_back(std::move(info));
            }
        }
    }

    if (keys.empty()) return;

    // Step 2: union-find over identity graph (direct identities first)
    DisjointSet dsu(keys.size());
    for (auto& [a, b] : direct_identity_edges) {
        auto ia = id_of.find(a);
        auto ib = id_of.find(b);
        if (ia != id_of.end() && ib != id_of.end()) {
            dsu.unite(ia->second, ib->second);
        }
    }

    // Step 3: phi-mediated identities fixed-point
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& phi : phi_infos) {
            auto it_dest = id_of.find(phi.dest);
            if (it_dest == id_of.end() || phi.operands.empty()) continue;

            auto it_first = id_of.find(phi.operands[0]);
            if (it_first == id_of.end()) continue;
            int first_root = dsu.find(it_first->second);

            bool all_same = true;
            for (const VarKey& op : phi.operands) {
                auto it = id_of.find(op);
                if (it == id_of.end() || dsu.find(it->second) != first_root) {
                    all_same = false;
                    break;
                }
            }
            if (!all_same) continue;

            Variable* dest_var = sample_of[phi.dest];
            Variable* src_var = sample_of[phi.operands[0]];
            if (!type_compatible_for_identity(dest_var, src_var)) continue;

            if (dsu.unite(it_dest->second, it_first->second)) {
                changed = true;
            }
        }
    }

    // Step 4: connected components -> representative variable
    std::unordered_map<int, std::vector<VarKey>> components;
    for (const VarKey& key : keys) {
        int root = dsu.find(id_of[key]);
        components[root].push_back(key);
    }

    std::unordered_map<VarKey, Variable*, VarKeyHash> replacement_for;
    for (auto& [root, members] : components) {
        (void)root;
        if (members.size() <= 1) continue;

        Variable* rep = representative_for_identity_component(members, sample_of);
        if (!rep) continue;

        for (const VarKey& k : members) {
            replacement_for[k] = rep;
        }
    }

    if (replacement_for.empty()) return;

    // Step 5: rewrite all instructions to representative variables
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            std::vector<Variable*> vars = inst->requirements();
            auto defs = inst->definitions();
            vars.insert(vars.end(), defs.begin(), defs.end());

            std::unordered_set<VarKey, VarKeyHash> seen;
            for (Variable* old_var : vars) {
                if (!old_var) continue;
                VarKey k = var_key(old_var);
                if (!seen.insert(k).second) continue;

                auto it_rep = replacement_for.find(k);
                if (it_rep != replacement_for.end() && it_rep->second != old_var) {
                    inst->substitute(old_var, it_rep->second);
                }
            }
        }
    }

    // Step 6: remove degenerate identity instructions and simplify identity phis
    for (BasicBlock* block : task.cfg()->blocks()) {
        std::vector<Instruction*> rewritten;
        rewritten.reserve(block->instructions().size());

        for (Instruction* inst : block->instructions()) {
            if (auto* phi = dynamic_cast<Phi*>(inst)) {
                Variable* dest = phi->dest_var();
                if (!dest || !phi->operand_list()) {
                    continue;
                }

                Variable* single = nullptr;
                bool all_same = true;
                for (Expression* op : phi->operand_list()->operands()) {
                    auto* ov = dynamic_cast<Variable*>(op);
                    if (!ov) {
                        all_same = false;
                        break;
                    }
                    if (var_key(ov) == var_key(dest)) continue;
                    if (!single) single = ov;
                    else if (var_key(single) != var_key(ov)) {
                        all_same = false;
                        break;
                    }
                }

                if (all_same) {
                    if (!single) {
                        // phi(dest, dest, ...) is a no-op
                        continue;
                    }
                    auto* repl = task.arena().create<Assignment>(dest, single);
                    repl->set_address(phi->address());
                    inst = repl;
                }
            }

            if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                auto* dst = dynamic_cast<Variable*>(assign->destination());
                auto* src = dynamic_cast<Variable*>(assign->value());
                if (dst && src && var_key(dst) == var_key(src)) {
                    continue;
                }
            }

            rewritten.push_back(inst);
        }

        block->set_instructions(std::move(rewritten));
    }
}

namespace {

struct ExistingSubexpressionDef {
    Variable* variable = nullptr;
    BasicBlock* block = nullptr;
    std::size_t index = 0;
};

static std::size_t expression_complexity(Expression* expr) {
    if (!expr) return 0;
    if (dynamic_cast<Constant*>(expr) || dynamic_cast<Variable*>(expr)) return 1;

    std::vector<Expression*> stack;
    stack.push_back(expr);
    std::size_t score = 0;

    while (!stack.empty()) {
        Expression* current = stack.back();
        stack.pop_back();
        ++score;

        if (auto* op = dynamic_cast<Operation*>(current)) {
            for (Expression* child : op->operands()) {
                stack.push_back(child);
            }
        } else if (auto* list = dynamic_cast<ListOperation*>(current)) {
            for (Expression* child : list->operands()) {
                stack.push_back(child);
            }
        }
    }

    return score;
}

static std::string expression_fingerprint(Expression* expr) {
    if (!expr) return "null";
    if (auto* c = dynamic_cast<Constant*>(expr)) {
        return "C:" + std::to_string(c->value()) + ":" + std::to_string(c->size_bytes);
    }
    if (auto* v = dynamic_cast<Variable*>(expr)) {
        return "V:" + v->name() + "#" + std::to_string(v->ssa_version());
    }
    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        std::string out = "L[";
        bool first = true;
        for (Expression* child : list->operands()) {
            if (!first) out += ",";
            first = false;
            out += expression_fingerprint(child);
        }
        out += "]";
        return out;
    }
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        std::string out = "O:" + std::to_string(static_cast<int>(op->type())) + "(";
        bool first = true;
        for (Expression* child : op->operands()) {
            if (!first) out += ",";
            first = false;
            out += expression_fingerprint(child);
        }
    }
    return "X";
}

static void collect_subexpressions(Expression* root, std::vector<Expression*>& out) {
    if (!root) return;
    out.push_back(root);
    if (auto* op = dynamic_cast<Operation*>(root)) {
        for (Expression* child : op->operands()) collect_subexpressions(child, out);
        return;
    }
    if (auto* list = dynamic_cast<ListOperation*>(root)) {
        for (Expression* child : list->operands()) collect_subexpressions(child, out);
    }
}

static bool contains_call_expression(Expression* expr) {
    if (!expr) return false;
    if (dynamic_cast<Call*>(expr)) return true;
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        if (op->type() == OperationType::call) return true;
        for (Expression* child : op->operands()) {
            if (contains_call_expression(child)) return true;
        }
    }
    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression* child : list->operands()) {
            if (contains_call_expression(child)) return true;
        }
    }
    return false;
}

static std::vector<Expression*> instruction_subexpressions(Instruction* inst) {
    std::vector<Expression*> out;

    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
        collect_subexpressions(assign->value(), out);
        if (assign->destination() && !dynamic_cast<Variable*>(assign->destination())) {
            if (auto* op = dynamic_cast<Operation*>(assign->destination())) {
                for (Expression* child : op->operands()) collect_subexpressions(child, out);
            } else if (auto* list = dynamic_cast<ListOperation*>(assign->destination())) {
                for (Expression* child : list->operands()) collect_subexpressions(child, out);
            }
        }
        return out;
    }

    if (auto* branch = dynamic_cast<Branch*>(inst)) {
        if (branch->condition()) {
            for (Expression* child : branch->condition()->operands()) collect_subexpressions(child, out);
        }
        return out;
    }

    if (auto* ib = dynamic_cast<IndirectBranch*>(inst)) {
        collect_subexpressions(ib->expression(), out);
        return out;
    }

    if (auto* ret = dynamic_cast<Return*>(inst)) {
        for (Expression* value : ret->values()) collect_subexpressions(value, out);
    }

    return out;
}

static bool can_replace_subexpression(
    const ExistingSubexpressionDef& def,
    BasicBlock* use_block,
    std::size_t use_index) {
    if (!def.variable || !def.block) return false;

    // Conservative alias safety: only in same block and no Relation in-between.
    if (!def.variable->is_aliased()) return true;
    if (def.block != use_block || use_index <= def.index) return false;

    const auto& insts = use_block->instructions();
    for (std::size_t i = def.index + 1; i < use_index && i < insts.size(); ++i) {
        if (auto* rel = dynamic_cast<Relation*>(insts[i])) {
            auto* dst = rel->destination();
            if (dst && dst->name() == def.variable->name()) return false;
        }
    }
    return true;
}

class ExistingSubexpressionReplacer {
public:
    ExistingSubexpressionReplacer(ControlFlowGraph& cfg, DominatorTree& dom)
        : cfg_(cfg), dom_(dom) {}

    void run() {
        if (!cfg_.entry_block()) return;
        process_block(cfg_.entry_block());
    }

private:
    void process_block(BasicBlock* block) {
        if (!block) return;

        std::vector<std::string> inserted_keys;

        auto& insts = block->mutable_instructions();
        for (std::size_t idx = 0; idx < insts.size(); ++idx) {
            Instruction* inst = insts[idx];
            if (!inst) continue;

            bool changed = true;
            while (changed) {
                changed = false;

                std::vector<Expression*> subexprs = instruction_subexpressions(inst);
                std::unordered_set<Expression*> uniq(subexprs.begin(), subexprs.end());
                subexprs.assign(uniq.begin(), uniq.end());

                std::sort(subexprs.begin(), subexprs.end(), [](Expression* a, Expression* b) {
                    return expression_complexity(a) > expression_complexity(b);
                });

                for (Expression* subexpr : subexprs) {
                    std::string fp = expression_fingerprint(subexpr);
                    auto it = defining_variable_of_.find(fp);
                    if (it == defining_variable_of_.end() || it->second.empty()) continue;

                    const ExistingSubexpressionDef& def = it->second.back();
                    if (!can_replace_subexpression(def, block, idx)) continue;

                    inst->substitute(subexpr, def.variable);
                    changed = true;
                    break;
                }
            }

            auto* assign = dynamic_cast<Assignment*>(inst);
            if (!assign || dynamic_cast<Phi*>(inst)) continue;

            auto* dest = dynamic_cast<Variable*>(assign->destination());
            Expression* value = assign->value();
            if (!dest || !value) continue;
            if (expression_complexity(value) <= 1) continue;
            if (contains_dereference(value) || contains_call_expression(value)) continue;

            std::string fp = expression_fingerprint(value);
            if (defining_variable_of_.contains(fp) && !defining_variable_of_[fp].empty()) continue;

            defining_variable_of_[fp].push_back({dest, block, idx});
            inserted_keys.push_back(fp);
        }

        for (BasicBlock* child : dom_.children(block)) {
            process_block(child);
        }

        for (auto it = inserted_keys.rbegin(); it != inserted_keys.rend(); ++it) {
            auto map_it = defining_variable_of_.find(*it);
            if (map_it == defining_variable_of_.end() || map_it->second.empty()) continue;
            map_it->second.pop_back();
            if (map_it->second.empty()) {
                defining_variable_of_.erase(map_it);
            }
        }
    }

private:
    ControlFlowGraph& cfg_;
    DominatorTree& dom_;
    std::unordered_map<std::string, std::vector<ExistingSubexpressionDef>> defining_variable_of_;
};

struct SubexpressionUsage {
    Expression* expr = nullptr;
    Instruction* instruction = nullptr;
    BasicBlock* block = nullptr;
    std::size_t index = 0;
};

struct SubexpressionStats {
    Expression* exemplar = nullptr;
    std::size_t complexity = 0;
    std::vector<SubexpressionUsage> usages;
};

class DefinitionGenerator {
public:
    DefinitionGenerator(DecompilerTask& task, ControlFlowGraph& cfg, DominatorTree& dom)
        : task_(task), cfg_(cfg), dom_(dom) {}

    void run() {
        bool applied = true;
        while (applied) {
            applied = false;

            auto usage_map = collect_usage_map();
            auto candidates = sorted_candidates(usage_map);

            for (const std::string& fp : candidates) {
                auto it = usage_map.find(fp);
                if (it == usage_map.end()) continue;

                if (apply_candidate(fp, it->second)) {
                    applied = true;
                    break;
                }
            }
        }
    }

private:
    std::unordered_map<std::string, SubexpressionStats> collect_usage_map() {
        std::unordered_map<std::string, SubexpressionStats> usage_map;

        for (BasicBlock* block : cfg_.blocks()) {
            const auto& insts = block->instructions();
            for (std::size_t idx = 0; idx < insts.size(); ++idx) {
                Instruction* inst = insts[idx];
                if (!inst) continue;

                std::vector<Expression*> subexprs = instruction_subexpressions(inst);
                for (Expression* expr : subexprs) {
                    if (!expr) continue;
                    std::string fp = expression_fingerprint(expr);
                    auto& stats = usage_map[fp];
                    if (!stats.exemplar) {
                        stats.exemplar = expr;
                        stats.complexity = expression_complexity(expr);
                    }
                    stats.usages.push_back({expr, inst, block, idx});
                }
            }
        }

        return usage_map;
    }

    std::vector<std::string> sorted_candidates(const std::unordered_map<std::string, SubexpressionStats>& usage_map) {
        std::vector<std::pair<std::string, const SubexpressionStats*>> rows;
        rows.reserve(usage_map.size());

        for (const auto& [fp, stats] : usage_map) {
            if (!is_candidate(stats)) continue;
            rows.push_back({fp, &stats});
        }

        std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.second->complexity != rhs.second->complexity) {
                return lhs.second->complexity > rhs.second->complexity;
            }
            if (lhs.second->usages.size() != rhs.second->usages.size()) {
                return lhs.second->usages.size() > rhs.second->usages.size();
            }
            return lhs.first < rhs.first;
        });

        std::vector<std::string> out;
        out.reserve(rows.size());
        for (auto& [fp, _] : rows) {
            (void)_;
            out.push_back(fp);
        }
        return out;
    }

    bool is_candidate(const SubexpressionStats& stats) const {
        if (!stats.exemplar) return false;
        if (stats.complexity < 2) return false;
        if (stats.usages.size() < 2) return false;
        if (dynamic_cast<ListOperation*>(stats.exemplar)) return false;
        if (contains_dereference(stats.exemplar)) return false;
        if (contains_call_expression(stats.exemplar)) return false;
        return true;
    }

    static bool replace_in_expression(Expression*& expr,
                                      const std::string& target_fp,
                                      Variable* replacement) {
        if (!expr) return false;

        if (expression_fingerprint(expr) == target_fp) {
            expr = replacement;
            return true;
        }

        bool changed = false;
        if (auto* op = dynamic_cast<Operation*>(expr)) {
            for (Expression*& child : op->mutable_operands()) {
                changed = replace_in_expression(child, target_fp, replacement) || changed;
            }
        } else if (auto* list = dynamic_cast<ListOperation*>(expr)) {
            for (Expression*& child : list->mutable_operands()) {
                changed = replace_in_expression(child, target_fp, replacement) || changed;
            }
        }

        return changed;
    }

    static bool rewrite_instruction(Instruction* inst,
                                    const std::string& target_fp,
                                    Variable* replacement) {
        bool changed = false;

        if (auto* assign = dynamic_cast<Assignment*>(inst)) {
            Expression* value = assign->value();
            if (replace_in_expression(value, target_fp, replacement)) {
                assign->set_value(value);
                changed = true;
            }

            if (assign->destination() && !dynamic_cast<Variable*>(assign->destination())) {
                Expression* dest = assign->destination();
                if (replace_in_expression(dest, target_fp, replacement)) {
                    assign->set_destination(dest);
                    changed = true;
                }
            }
            return changed;
        }

        if (auto* branch = dynamic_cast<Branch*>(inst)) {
            if (auto* cond = branch->condition()) {
                for (Expression*& child : cond->mutable_operands()) {
                    changed = replace_in_expression(child, target_fp, replacement) || changed;
                }
            }
            return changed;
        }

        if (auto* ib = dynamic_cast<IndirectBranch*>(inst)) {
            Expression* before = ib->expression();
            Expression* after = before;
            if (replace_in_expression(after, target_fp, replacement)) {
                if (after != before) {
                    ib->substitute(before, after);
                }
                changed = true;
            }
            return changed;
        }

        if (auto* ret = dynamic_cast<Return*>(inst)) {
            for (Expression* value : ret->values()) {
                Expression* rewritten = value;
                if (replace_in_expression(rewritten, target_fp, replacement)) {
                    if (rewritten != value) {
                        ret->substitute(value, rewritten);
                    }
                    changed = true;
                }
            }
            return changed;
        }

        return false;
    }

    BasicBlock* common_dominator_of(const std::vector<BasicBlock*>& blocks) const {
        if (blocks.empty()) return nullptr;

        BasicBlock* candidate = blocks.front();
        for (std::size_t i = 1; i < blocks.size(); ++i) {
            BasicBlock* target = blocks[i];
            while (candidate && !dom_.dominates(candidate, target)) {
                candidate = dom_.idom(candidate);
            }
            if (!candidate) return nullptr;
        }

        for (BasicBlock* target : blocks) {
            if (!dom_.dominates(candidate, target)) {
                return nullptr;
            }
        }

        return candidate;
    }

    static std::size_t insertion_index_for(BasicBlock* block, const std::vector<SubexpressionUsage>& usages) {
        std::size_t min_idx = static_cast<std::size_t>(-1);
        for (const auto& usage : usages) {
            if (usage.block == block) {
                min_idx = std::min(min_idx, usage.index);
            }
        }

        if (min_idx != static_cast<std::size_t>(-1)) {
            return min_idx;
        }

        const auto& insts = block->instructions();
        if (insts.empty()) return 0;
        Instruction* last = insts.back();
        if (is_branch(last) || is_return(last)) {
            return insts.size() - 1;
        }
        return insts.size();
    }

    bool apply_candidate(const std::string& fp, const SubexpressionStats& stats) {
        if (!stats.exemplar) return false;

        std::unordered_set<BasicBlock*> uniq;
        std::vector<BasicBlock*> usage_blocks;
        for (const auto& usage : stats.usages) {
            if (!usage.instruction || !usage.block) continue;
            if (dynamic_cast<Phi*>(usage.instruction)) {
                return false;
            }
            if (uniq.insert(usage.block).second) {
                usage_blocks.push_back(usage.block);
            }
        }
        if (usage_blocks.empty()) return false;

        BasicBlock* insertion_block = common_dominator_of(usage_blocks);
        if (!insertion_block) return false;

        std::size_t insert_idx = insertion_index_for(insertion_block, stats.usages);

        const std::size_t width = stats.exemplar->size_bytes > 0 ? stats.exemplar->size_bytes : 4;
        auto* temp = task_.arena().create<Variable>("cse_" + std::to_string(temp_counter_++), width);
        temp->set_ssa_version(0);
        temp->set_ir_type(stats.exemplar->ir_type());

        auto* def_value = stats.exemplar->copy(task_.arena());
        auto* def_inst = task_.arena().create<Assignment>(temp, def_value);

        auto insts = insertion_block->instructions();
        if (insert_idx > insts.size()) insert_idx = insts.size();
        insts.insert(insts.begin() + static_cast<std::ptrdiff_t>(insert_idx), def_inst);
        insertion_block->set_instructions(std::move(insts));

        bool any_rewrite = false;
        std::unordered_set<Instruction*> touched;
        for (const auto& usage : stats.usages) {
            if (!usage.instruction || usage.instruction == def_inst) continue;
            if (!touched.insert(usage.instruction).second) continue;
            any_rewrite = rewrite_instruction(usage.instruction, fp, temp) || any_rewrite;
        }

        if (!any_rewrite) {
            // Roll back: remove inserted temp definition if no usage changed.
            auto rollback = insertion_block->instructions();
            rollback.erase(std::remove(rollback.begin(), rollback.end(), def_inst), rollback.end());
            insertion_block->set_instructions(std::move(rollback));
            return false;
        }

        return true;
    }

private:
    DecompilerTask& task_;
    ControlFlowGraph& cfg_;
    DominatorTree& dom_;
    int temp_counter_ = 0;
};

} // namespace

namespace {

struct FoldedConstant {
    uint64_t value = 0;
    std::size_t size_bytes = 0;
};

uint64_t mask_for_size(std::size_t size_bytes) {
    if (size_bytes == 0 || size_bytes >= 8) {
        return ~0ULL;
    }
    const std::size_t bits = size_bytes * 8;
    return (1ULL << bits) - 1ULL;
}

int64_t as_signed(uint64_t value, std::size_t size_bytes) {
    if (size_bytes == 0 || size_bytes >= 8) {
        return static_cast<int64_t>(value);
    }

    const uint64_t mask = mask_for_size(size_bytes);
    value &= mask;
    const uint64_t sign = 1ULL << (size_bytes * 8 - 1);
    if (value & sign) {
        return static_cast<int64_t>(value | ~mask);
    }
    return static_cast<int64_t>(value);
}

bool is_boolean_result_op(OperationType type) {
    switch (type) {
        case OperationType::logical_and:
        case OperationType::logical_or:
        case OperationType::eq:
        case OperationType::neq:
        case OperationType::lt:
        case OperationType::le:
        case OperationType::gt:
        case OperationType::ge:
        case OperationType::lt_us:
        case OperationType::le_us:
        case OperationType::gt_us:
        case OperationType::ge_us:
            return true;
        default:
            return false;
    }
}

bool is_constant_value(const Expression* expr, uint64_t value) {
    const auto* c = dynamic_cast<const Constant*>(expr);
    return c && c->value() == value;
}

bool is_all_ones_constant(const Expression* expr, std::size_t size_bytes) {
    const auto* c = dynamic_cast<const Constant*>(expr);
    if (!c) return false;
    const std::size_t width = size_bytes == 0 ? c->size_bytes : size_bytes;
    return c->value() == mask_for_size(width);
}

Constant* make_simplified_constant(DecompilerTask& task, const Operation* op, uint64_t value) {
    const std::size_t out_size = op && op->size_bytes != 0 ? op->size_bytes : 1;
    auto* c = task.arena().create<Constant>(value, out_size);
    if (op) {
        c->set_ir_type(op->ir_type());
    }
    return c;
}

std::optional<FoldedConstant> collapse_binary_constants(
    OperationType type,
    const Constant* lhs,
    const Constant* rhs,
    std::size_t out_size_bytes);

Expression* try_collapse_nested_constants(DecompilerTask& task, Operation* op) {
    if (!op || op->operands().size() != 2) return nullptr;
    if (op->type() != OperationType::add && op->type() != OperationType::mul &&
        op->type() != OperationType::mul_us) {
        return nullptr;
    }

    auto try_orientation = [&](Expression* inner_candidate, Expression* outer_constant_candidate) -> Expression* {
        auto* outer_const = dynamic_cast<Constant*>(outer_constant_candidate);
        if (!outer_const) return nullptr;

        auto* inner = dynamic_cast<Operation*>(inner_candidate);
        if (!inner || inner->type() != op->type() || inner->operands().size() != 2) {
            return nullptr;
        }

        Constant* inner_const = dynamic_cast<Constant*>(inner->operands()[0]);
        Expression* inner_other = inner->operands()[1];
        if (!inner_const) {
            inner_const = dynamic_cast<Constant*>(inner->operands()[1]);
            inner_other = inner->operands()[0];
        }
        if (!inner_const) return nullptr;

        auto folded = collapse_binary_constants(op->type(), inner_const, outer_const, op->size_bytes);
        if (!folded) return nullptr;

        auto* merged_const = task.arena().create<Constant>(folded->value, folded->size_bytes);
        merged_const->set_ir_type(inner_const->ir_type());

        auto* rebuilt = task.arena().create<Operation>(
            op->type(),
            std::vector<Expression*>{inner_other, merged_const},
            op->size_bytes);
        rebuilt->set_ir_type(op->ir_type());
        return rebuilt;
    };

    if (Expression* first = try_orientation(op->operands()[0], op->operands()[1])) {
        return first;
    }
    return try_orientation(op->operands()[1], op->operands()[0]);
}

std::optional<FoldedConstant> collapse_binary_constants(
    OperationType type,
    const Constant* lhs,
    const Constant* rhs,
    std::size_t out_size_bytes) {
    if (!lhs || !rhs) return std::nullopt;

    const uint64_t l = lhs->value();
    const uint64_t r = rhs->value();
    const std::size_t size = is_boolean_result_op(type) ? 1 : (out_size_bytes == 0 ? lhs->size_bytes : out_size_bytes);
    const uint64_t mask = mask_for_size(size);

    auto make = [&](uint64_t value) -> std::optional<FoldedConstant> {
        return FoldedConstant{value & mask, size};
    };

    switch (type) {
        case OperationType::add:
        case OperationType::add_with_carry:
            return make(l + r);
        case OperationType::sub:
        case OperationType::sub_with_carry:
            return make(l - r);
        case OperationType::mul:
        case OperationType::mul_us:
            return make(l * r);
        case OperationType::div_us:
            if (r == 0) return std::nullopt;
            return make(l / r);
        case OperationType::mod_us:
            if (r == 0) return std::nullopt;
            return make(l % r);
        case OperationType::div: {
            const int64_t sl = as_signed(l, size);
            const int64_t sr = as_signed(r, size);
            if (sr == 0) return std::nullopt;
            if (sl == std::numeric_limits<int64_t>::min() && sr == -1) return std::nullopt;
            return make(static_cast<uint64_t>(sl / sr));
        }
        case OperationType::mod: {
            const int64_t sl = as_signed(l, size);
            const int64_t sr = as_signed(r, size);
            if (sr == 0) return std::nullopt;
            if (sl == std::numeric_limits<int64_t>::min() && sr == -1) return make(0);
            return make(static_cast<uint64_t>(sl % sr));
        }
        case OperationType::bit_and:
            return make(l & r);
        case OperationType::bit_or:
            return make(l | r);
        case OperationType::bit_xor:
            return make(l ^ r);
        case OperationType::shl: {
            const uint64_t shift = r & 63ULL;
            return make(l << shift);
        }
        case OperationType::shr:
        case OperationType::sar: {
            const uint64_t shift = r & 63ULL;
            const int64_t sl = as_signed(l, size);
            return make(static_cast<uint64_t>(sl >> shift));
        }
        case OperationType::shr_us: {
            const uint64_t shift = r & 63ULL;
            return make(l >> shift);
        }
        case OperationType::logical_and:
            return make((l != 0 && r != 0) ? 1 : 0);
        case OperationType::logical_or:
            return make((l != 0 || r != 0) ? 1 : 0);
        case OperationType::eq:
            return make(l == r ? 1 : 0);
        case OperationType::neq:
            return make(l != r ? 1 : 0);
        case OperationType::lt:
            return make(as_signed(l, size) < as_signed(r, size) ? 1 : 0);
        case OperationType::le:
            return make(as_signed(l, size) <= as_signed(r, size) ? 1 : 0);
        case OperationType::gt:
            return make(as_signed(l, size) > as_signed(r, size) ? 1 : 0);
        case OperationType::ge:
            return make(as_signed(l, size) >= as_signed(r, size) ? 1 : 0);
        case OperationType::lt_us:
            return make(l < r ? 1 : 0);
        case OperationType::le_us:
            return make(l <= r ? 1 : 0);
        case OperationType::gt_us:
            return make(l > r ? 1 : 0);
        case OperationType::ge_us:
            return make(l >= r ? 1 : 0);
        case OperationType::power: {
            uint64_t base = l;
            uint64_t exp = r;
            uint64_t acc = 1;
            while (exp > 0) {
                if (exp & 1ULL) acc *= base;
                base *= base;
                exp >>= 1ULL;
            }
            return make(acc);
        }
        default:
            return std::nullopt;
    }
}

Expression* simplify_expression_tree(DecompilerTask& task, Expression* expr) {
    if (!expr) return nullptr;
    if (dynamic_cast<Constant*>(expr) || dynamic_cast<Variable*>(expr)) return expr;

    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression*& child : list->mutable_operands()) {
            child = simplify_expression_tree(task, child);
        }
        return expr;
    }

    auto* op = dynamic_cast<Operation*>(expr);
    if (!op) return expr;

    for (Expression*& child : op->mutable_operands()) {
        child = simplify_expression_tree(task, child);
    }

    if (op->operands().size() == 2) {
        Expression* lhs_expr = op->operands()[0];
        Expression* rhs_expr = op->operands()[1];

        switch (op->type()) {
            case OperationType::add:
                if (is_constant_value(rhs_expr, 0)) return lhs_expr;
                if (is_constant_value(lhs_expr, 0)) return rhs_expr;
                break;
            case OperationType::mul:
            case OperationType::mul_us:
                if (is_constant_value(lhs_expr, 0) || is_constant_value(rhs_expr, 0)) {
                    return make_simplified_constant(task, op, 0);
                }
                if (is_constant_value(rhs_expr, 1)) return lhs_expr;
                if (is_constant_value(lhs_expr, 1)) return rhs_expr;
                break;
            case OperationType::sub:
                if (auto* rhs_op = dynamic_cast<Operation*>(rhs_expr);
                    rhs_op && rhs_op->type() == OperationType::negate && rhs_op->operands().size() == 1) {
                    if (auto* negated_const = dynamic_cast<Constant*>(rhs_op->operands()[0])) {
                        auto* as_add = task.arena().create<Operation>(
                            OperationType::add,
                            std::vector<Expression*>{lhs_expr, negated_const},
                            op->size_bytes);
                        as_add->set_ir_type(op->ir_type());
                        return as_add;
                    }
                }
                if (is_constant_value(rhs_expr, 0)) return lhs_expr;
                break;
            case OperationType::div:
            case OperationType::div_us:
                if (is_constant_value(rhs_expr, 1)) return lhs_expr;
                break;
            case OperationType::bit_and:
                if (is_constant_value(lhs_expr, 0) || is_constant_value(rhs_expr, 0)) {
                    return make_simplified_constant(task, op, 0);
                }
                if (is_all_ones_constant(lhs_expr, op->size_bytes)) return rhs_expr;
                if (is_all_ones_constant(rhs_expr, op->size_bytes)) return lhs_expr;
                break;
            case OperationType::bit_or:
                if (is_constant_value(lhs_expr, 0)) return rhs_expr;
                if (is_constant_value(rhs_expr, 0)) return lhs_expr;
                break;
            case OperationType::bit_xor:
                if (is_constant_value(lhs_expr, 0)) return rhs_expr;
                if (is_constant_value(rhs_expr, 0)) return lhs_expr;
                break;
            default:
                break;
        }

        if (Expression* nested = try_collapse_nested_constants(task, op)) {
            return nested;
        }

        auto* lhs = dynamic_cast<Constant*>(op->operands()[0]);
        auto* rhs = dynamic_cast<Constant*>(op->operands()[1]);
        if (lhs && rhs) {
            if (auto folded = collapse_binary_constants(op->type(), lhs, rhs, op->size_bytes)) {
                auto* c = task.arena().create<Constant>(folded->value, folded->size_bytes);
                c->set_ir_type(expr->ir_type());
                return c;
            }
        }
    }

    return expr;
}

void simplify_instruction_constants(DecompilerTask& task, Instruction* inst) {
    if (!inst) return;

    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
        Expression* new_value = simplify_expression_tree(task, assign->value());
        if (new_value != assign->value()) {
            assign->set_value(new_value);
        }

        if (!dynamic_cast<Variable*>(assign->destination())) {
            Expression* new_dest = simplify_expression_tree(task, assign->destination());
            if (new_dest != assign->destination()) {
                assign->set_destination(new_dest);
            }
        }
        return;
    }

    if (auto* branch = dynamic_cast<Branch*>(inst)) {
        Condition* cond = branch->condition();
        if (!cond) return;

        Expression* lhs = simplify_expression_tree(task, cond->lhs());
        Expression* rhs = simplify_expression_tree(task, cond->rhs());
        auto* rebuilt = task.arena().create<Condition>(cond->type(), lhs, rhs, cond->size_bytes);

        auto* cl = dynamic_cast<Constant*>(lhs);
        auto* cr = dynamic_cast<Constant*>(rhs);
        if (cl && cr) {
            if (auto folded = collapse_binary_constants(cond->type(), cl, cr, 1)) {
                auto* folded_const = task.arena().create<Constant>(folded->value, 1);
                auto* zero = task.arena().create<Constant>(0, 1);
                rebuilt = task.arena().create<Condition>(OperationType::neq, folded_const, zero, 1);
            }
        }

        branch->set_condition(rebuilt);
        return;
    }

    if (auto* ib = dynamic_cast<IndirectBranch*>(inst)) {
        Expression* expr = ib->expression();
        Expression* simplified = simplify_expression_tree(task, expr);
        if (simplified != expr) {
            ib->substitute(expr, simplified);
        }
        return;
    }

    if (auto* ret = dynamic_cast<Return*>(inst)) {
        for (Expression* value : ret->values()) {
            Expression* simplified = simplify_expression_tree(task, value);
            if (simplified != value) {
                ret->substitute(value, simplified);
            }
        }
    }
}

Expression* simplify_cast_expression_tree(DecompilerTask& task, Expression* expr) {
    if (!expr) return nullptr;
    if (dynamic_cast<Constant*>(expr) || dynamic_cast<Variable*>(expr)) return expr;

    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression*& child : list->mutable_operands()) {
            child = simplify_cast_expression_tree(task, child);
        }
        return expr;
    }

    auto* op = dynamic_cast<Operation*>(expr);
    if (!op) return expr;

    for (Expression*& child : op->mutable_operands()) {
        child = simplify_cast_expression_tree(task, child);
    }

    if (op->type() != OperationType::cast || op->operands().size() != 1) {
        return expr;
    }

    Expression* src = op->operands()[0];
    const TypePtr& target_type = op->ir_type();
    const TypePtr& src_type = src->ir_type();

    // Rule 1: Remove same-type casts.
    if (target_type && src_type && (*target_type == *src_type)) {
        return src;
    }

    // Rule 2: Fold constant-to-constant casts.
    if (auto* c = dynamic_cast<Constant*>(src)) {
        uint64_t value = c->value();
        std::size_t out_size = op->size_bytes != 0 ? op->size_bytes : c->size_bytes;

        if (target_type) {
            if (target_type->is_boolean()) {
                value = value == 0 ? 0 : 1;
            } else {
                const std::size_t bits = target_type->size();
                if (bits > 0 && bits < 64) {
                    value &= ((1ULL << bits) - 1ULL);
                }
            }
            if (target_type->size_bytes() > 0) {
                out_size = target_type->size_bytes();
            }
        } else if (out_size > 0 && out_size < 8) {
            value &= mask_for_size(out_size);
        }

        auto* folded = task.arena().create<Constant>(value, out_size == 0 ? c->size_bytes : out_size);
        folded->set_ir_type(target_type ? target_type : c->ir_type());
        return folded;
    }

    return expr;
}

void simplify_instruction_casts(DecompilerTask& task, Instruction* inst) {
    if (!inst) return;

    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
        Expression* new_value = simplify_cast_expression_tree(task, assign->value());
        if (new_value != assign->value()) {
            assign->set_value(new_value);
        }

        if (!dynamic_cast<Variable*>(assign->destination())) {
            Expression* new_dest = simplify_cast_expression_tree(task, assign->destination());
            if (new_dest != assign->destination()) {
                assign->set_destination(new_dest);
            }
        }
        return;
    }

    if (auto* branch = dynamic_cast<Branch*>(inst)) {
        Condition* cond = branch->condition();
        if (!cond) return;
        Expression* lhs = simplify_cast_expression_tree(task, cond->lhs());
        Expression* rhs = simplify_cast_expression_tree(task, cond->rhs());
        branch->set_condition(task.arena().create<Condition>(cond->type(), lhs, rhs, cond->size_bytes));
        return;
    }

    if (auto* ib = dynamic_cast<IndirectBranch*>(inst)) {
        Expression* expr = ib->expression();
        Expression* simplified = simplify_cast_expression_tree(task, expr);
        if (simplified != expr) {
            ib->substitute(expr, simplified);
        }
        return;
    }

    if (auto* ret = dynamic_cast<Return*>(inst)) {
        for (Expression* value : ret->values()) {
            Expression* simplified = simplify_cast_expression_tree(task, value);
            if (simplified != value) {
                ret->substitute(value, simplified);
            }
        }
    }
}

} // namespace

namespace {

struct ExpressionGraph {
    std::unordered_map<Instruction*, std::vector<Instruction*>> dependencies;
    std::unordered_set<Instruction*> sinks;
};

bool assignment_has_call_value(const Assignment* assign) {
    if (!assign) return false;
    if (dynamic_cast<const Call*>(assign->value())) return true;
    const auto* op = dynamic_cast<const Operation*>(assign->value());
    return op && op->type() == OperationType::call;
}

bool assignment_has_side_effect_destination(const Assignment* assign) {
    if (!assign) return false;
    // Any non-variable destination is treated as a side-effecting write
    // (e.g., dereference/member store). Keep it conservatively.
    return dynamic_cast<const Variable*>(assign->destination()) == nullptr;
}

ExpressionGraph build_expression_graph(ControlFlowGraph* cfg) {
    ExpressionGraph graph;
    if (!cfg) return graph;

    std::unordered_map<VarKey, Instruction*, VarKeyHash> definition_of;

    // Pass 1: collect variable definitions.
    for (BasicBlock* block : cfg->blocks()) {
        for (Instruction* inst : block->instructions()) {
            for (Variable* def : inst->definitions()) {
                if (!def) continue;
                definition_of[var_key(def)] = inst;
            }
        }
    }

    // Pass 2: connect use -> def dependencies and identify sink instructions.
    for (BasicBlock* block : cfg->blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                if (assignment_has_call_value(assign) || assignment_has_side_effect_destination(assign)) {
                    graph.sinks.insert(inst);
                }
            } else {
                graph.sinks.insert(inst);
            }

            std::vector<Instruction*> deps;
            for (Variable* req : inst->requirements()) {
                if (!req) continue;
                auto it = definition_of.find(var_key(req));
                if (it != definition_of.end() && it->second != inst) {
                    deps.push_back(it->second);
                }
            }
            if (!deps.empty()) {
                std::sort(deps.begin(), deps.end());
                deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
                graph.dependencies[inst] = std::move(deps);
            }
        }
    }

    return graph;
}

std::unordered_set<Instruction*> compute_live_component(const ExpressionGraph& graph) {
    std::unordered_set<Instruction*> live;
    std::vector<Instruction*> worklist(graph.sinks.begin(), graph.sinks.end());

    while (!worklist.empty()) {
        Instruction* current = worklist.back();
        worklist.pop_back();
        if (!current || !live.insert(current).second) continue;

        auto it = graph.dependencies.find(current);
        if (it == graph.dependencies.end()) continue;
        for (Instruction* dep : it->second) {
            if (dep && !live.contains(dep)) {
                worklist.push_back(dep);
            }
        }
    }

    return live;
}

bool extract_scaled_index(Expression* expr, Expression*& index, std::size_t& element_size) {
    if (!expr) return false;

    if (auto* mul = dynamic_cast<Operation*>(expr);
        mul && (mul->type() == OperationType::mul || mul->type() == OperationType::mul_us) &&
        mul->operands().size() == 2) {
        Expression* lhs = mul->operands()[0];
        Expression* rhs = mul->operands()[1];

        auto* lhs_const = dynamic_cast<Constant*>(lhs);
        auto* rhs_const = dynamic_cast<Constant*>(rhs);

        if (lhs_const && !rhs_const) {
            const std::uint64_t scale = lhs_const->value();
            if (scale == 0 || scale > std::numeric_limits<std::size_t>::max()) return false;
            index = rhs;
            element_size = static_cast<std::size_t>(scale);
            return true;
        }
        if (rhs_const && !lhs_const) {
            const std::uint64_t scale = rhs_const->value();
            if (scale == 0 || scale > std::numeric_limits<std::size_t>::max()) return false;
            index = lhs;
            element_size = static_cast<std::size_t>(scale);
            return true;
        }
        return false;
    }

    // Treat base + idx as element_size = 1.
    index = expr;
    element_size = 1;
    return true;
}

bool extract_base_index(Expression* expr, Variable*& base, Expression*& index, std::size_t& element_size) {
    auto* add = dynamic_cast<Operation*>(expr);
    if (!add || add->type() != OperationType::add || add->operands().size() != 2) {
        return false;
    }

    auto try_form = [&](Expression* base_expr, Expression* index_expr) -> bool {
        auto* base_var = dynamic_cast<Variable*>(base_expr);
        if (!base_var) return false;

        Expression* idx = nullptr;
        std::size_t elem = 0;
        if (!extract_scaled_index(index_expr, idx, elem)) return false;

        base = base_var;
        index = idx;
        element_size = elem;
        return true;
    };

    return try_form(add->operands()[0], add->operands()[1]) ||
           try_form(add->operands()[1], add->operands()[0]);
}

bool infer_array_confidence(const Variable* base, std::size_t element_size) {
    if (!base || !base->ir_type()) return false;
    auto* ptr = dynamic_cast<const Pointer*>(base->ir_type().get());
    if (!ptr || !ptr->pointee()) return false;
    return ptr->pointee()->size_bytes() == element_size;
}

void annotate_array_access_expr(Expression* expr) {
    if (!expr) return;

    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression* child : list->operands()) {
            annotate_array_access_expr(child);
        }
        return;
    }

    auto* op = dynamic_cast<Operation*>(expr);
    if (!op) return;

    for (Expression* child : op->operands()) {
        annotate_array_access_expr(child);
    }

    if (op->type() != OperationType::deref || op->operands().size() != 1) {
        return;
    }

    Variable* base = nullptr;
    Expression* index = nullptr;
    std::size_t element_size = 0;
    if (!extract_base_index(op->operands()[0], base, index, element_size)) {
        return;
    }

    op->set_array_access(ArrayAccessInfo{
        .base = base,
        .index = index,
        .element_size = element_size,
        .confidence = infer_array_confidence(base, element_size),
    });
}

void annotate_array_access_inst(Instruction* inst) {
    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
        annotate_array_access_expr(assign->destination());
        annotate_array_access_expr(assign->value());
        return;
    }
    if (auto* branch = dynamic_cast<Branch*>(inst)) {
        annotate_array_access_expr(branch->condition());
        return;
    }
    if (auto* ib = dynamic_cast<IndirectBranch*>(inst)) {
        annotate_array_access_expr(ib->expression());
        return;
    }
    if (auto* ret = dynamic_cast<Return*>(inst)) {
        for (Expression* value : ret->values()) {
            annotate_array_access_expr(value);
        }
    }
}

Expression* strip_cast_expr(Expression* expr) {
    Expression* current = expr;
    while (auto* op = dynamic_cast<Operation*>(current)) {
        if (op->type() != OperationType::cast || op->operands().size() != 1 || op->operands()[0] == nullptr) {
            break;
        }
        current = op->operands()[0];
    }
    return current;
}

bool match_shift_one(Expression* expr, Expression*& amount_expr) {
    auto* op = dynamic_cast<Operation*>(strip_cast_expr(expr));
    if (!op || op->type() != OperationType::shl || op->operands().size() != 2) {
        return false;
    }

    auto* one = dynamic_cast<Constant*>(strip_cast_expr(op->operands()[0]));
    if (!one || one->value() != 1) {
        return false;
    }

    amount_expr = strip_cast_expr(op->operands()[1]);
    return amount_expr != nullptr;
}

bool match_bitfield_mask(Expression* expr, Expression*& amount_expr, std::uint64_t& mask, std::size_t& mask_bytes) {
    auto* op = dynamic_cast<Operation*>(strip_cast_expr(expr));
    if (!op || op->type() != OperationType::bit_and || op->operands().size() != 2) {
        return false;
    }

    auto try_form = [&](Expression* shift_side, Expression* mask_side) -> bool {
        Expression* amount = nullptr;
        if (!match_shift_one(shift_side, amount)) {
            return false;
        }

        auto* mask_const = dynamic_cast<Constant*>(strip_cast_expr(mask_side));
        if (!mask_const) {
            return false;
        }

        amount_expr = amount;
        mask = mask_const->value();
        mask_bytes = mask_const->size_bytes == 0 ? 8 : mask_const->size_bytes;
        return true;
    };

    return try_form(op->operands()[0], op->operands()[1]) ||
           try_form(op->operands()[1], op->operands()[0]);
}

Expression* build_unrolled_amount_checks(
    DecompilerTask& task,
    Expression* amount_expr,
    std::uint64_t mask,
    std::size_t mask_bytes) {
    if (!amount_expr) {
        return nullptr;
    }

    const std::size_t bit_width = std::min<std::size_t>(64, std::max<std::size_t>(1, mask_bytes * 8));
    std::vector<Expression*> checks;
    checks.reserve(bit_width);

    for (std::size_t bit = 0; bit < bit_width; ++bit) {
        if ((mask & (std::uint64_t{1} << bit)) == 0) {
            continue;
        }

        auto* bit_const = task.arena().create<Constant>(bit, amount_expr->size_bytes > 0 ? amount_expr->size_bytes : 4);
        auto* eq = task.arena().create<Condition>(OperationType::eq, amount_expr->copy(task.arena()), bit_const, 1);
        checks.push_back(eq);
    }

    if (checks.empty()) {
        return task.arena().create<Constant>(0, 1);
    }

    Expression* combined = checks.front();
    for (std::size_t i = 1; i < checks.size(); ++i) {
        combined = task.arena().create<Operation>(
            OperationType::logical_or,
            std::vector<Expression*>{combined, checks[i]},
            1);
    }
    return combined;
}

bool is_non_primitive_type(const TypePtr& type) {
    if (!type) {
        return false;
    }
    if (dynamic_cast<const UnknownType*>(type.get()) != nullptr) {
        return false;
    }
    if (dynamic_cast<const Integer*>(type.get()) != nullptr) {
        return false;
    }
    if (dynamic_cast<const Float*>(type.get()) != nullptr) {
        return false;
    }
    if (dynamic_cast<const CustomType*>(type.get()) != nullptr) {
        return false;
    }
    return true;
}

void collect_rhs_variables(Expression* expr, std::vector<Variable*>& out) {
    if (!expr) {
        return;
    }
    std::unordered_set<Variable*> seen;
    expr->collect_requirements(seen);
    out.reserve(out.size() + seen.size());
    for (Variable* var : seen) {
        if (var != nullptr) {
            out.push_back(var);
        }
    }
}

void add_type_graph_edge(
    const VarKey& lhs,
    const VarKey& rhs,
    std::unordered_map<VarKey, std::unordered_set<VarKey, VarKeyHash>, VarKeyHash>& graph) {
    graph[lhs].insert(rhs);
    graph[rhs].insert(lhs);
}

const TypePtr* find_dominant_non_primitive_type(
    const std::vector<VarKey>& component,
    const std::unordered_map<VarKey, std::vector<Variable*>, VarKeyHash>& occurrences,
    std::vector<std::pair<TypePtr, std::size_t>>& accumulator) {
    accumulator.clear();

    for (const VarKey& key : component) {
        auto it = occurrences.find(key);
        if (it == occurrences.end()) {
            continue;
        }
        for (Variable* var : it->second) {
            if (!var || !is_non_primitive_type(var->ir_type())) {
                continue;
            }

            bool matched = false;
            for (auto& [candidate, count] : accumulator) {
                if (candidate && var->ir_type() && *candidate == *var->ir_type()) {
                    ++count;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                accumulator.emplace_back(var->ir_type(), 1);
            }
        }
    }

    if (accumulator.empty()) {
        return nullptr;
    }

    std::sort(
        accumulator.begin(),
        accumulator.end(),
        [](const std::pair<TypePtr, std::size_t>& lhs, const std::pair<TypePtr, std::size_t>& rhs) {
            if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
            }
            const std::string lhs_name = lhs.first ? lhs.first->to_string() : std::string();
            const std::string rhs_name = rhs.first ? rhs.first->to_string() : std::string();
            return lhs_name < rhs_name;
        });

    return &accumulator.front().first;
}

bool try_unroll_bitfield_branch_condition(DecompilerTask& task, Branch* branch) {
    if (!branch || !branch->condition()) {
        return false;
    }

    Condition* cond = branch->condition();
    if (cond->type() != OperationType::eq && cond->type() != OperationType::neq) {
        return false;
    }

    auto is_zero_const = [](Expression* expr) -> bool {
        auto* c = dynamic_cast<Constant*>(strip_cast_expr(expr));
        return c != nullptr && c->value() == 0;
    };

    Expression* tested_expr = nullptr;
    if (is_zero_const(cond->lhs())) {
        tested_expr = cond->rhs();
    } else if (is_zero_const(cond->rhs())) {
        tested_expr = cond->lhs();
    } else {
        return false;
    }

    Expression* amount_expr = nullptr;
    std::uint64_t mask = 0;
    std::size_t mask_bytes = 0;
    if (!match_bitfield_mask(tested_expr, amount_expr, mask, mask_bytes)) {
        return false;
    }

    Expression* unrolled = build_unrolled_amount_checks(task, amount_expr, mask, mask_bytes);
    if (!unrolled) {
        return false;
    }

    auto* zero = task.arena().create<Constant>(0, 1);
    auto* replacement = task.arena().create<Condition>(cond->type(), unrolled, zero, 1);
    branch->set_condition(replacement);
    return true;
}

} // namespace

void TypePropagationStage::execute(DecompilerTask& task) {
    if (!task.cfg()) {
        return;
    }

    std::unordered_map<VarKey, std::unordered_set<VarKey, VarKeyHash>, VarKeyHash> type_graph;
    std::unordered_map<VarKey, std::vector<Variable*>, VarKeyHash> occurrences;

    auto note_variable = [&](Variable* var) {
        if (!var) {
            return;
        }
        VarKey key = var_key(var);
        occurrences[key].push_back(var);
        if (!type_graph.contains(key)) {
            type_graph.emplace(key, std::unordered_set<VarKey, VarKeyHash>{});
        }
    };

    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            for (Variable* req : inst->requirements()) {
                note_variable(req);
            }
            for (Variable* def : inst->definitions()) {
                note_variable(def);
            }

            auto* assign = dynamic_cast<Assignment*>(inst);
            auto* dst = assign ? dynamic_cast<Variable*>(assign->destination()) : nullptr;
            if (!dst) {
                continue;
            }

            std::vector<Variable*> rhs_vars;
            collect_rhs_variables(assign->value(), rhs_vars);
            for (Variable* rhs_var : rhs_vars) {
                if (!rhs_var) {
                    continue;
                }
                note_variable(rhs_var);
                add_type_graph_edge(var_key(dst), var_key(rhs_var), type_graph);
            }
        }
    }

    std::unordered_set<VarKey, VarKeyHash> visited;
    std::vector<VarKey> stack;
    std::vector<VarKey> component;
    std::vector<std::pair<TypePtr, std::size_t>> type_counts;

    for (const auto& [start, _] : type_graph) {
        if (visited.contains(start)) {
            continue;
        }

        component.clear();
        stack.clear();
        stack.push_back(start);
        visited.insert(start);

        while (!stack.empty()) {
            VarKey current = stack.back();
            stack.pop_back();
            component.push_back(current);

            auto it = type_graph.find(current);
            if (it == type_graph.end()) {
                continue;
            }
            for (const VarKey& neighbor : it->second) {
                if (!visited.contains(neighbor)) {
                    visited.insert(neighbor);
                    stack.push_back(neighbor);
                }
            }
        }

        const TypePtr* dominant = find_dominant_non_primitive_type(component, occurrences, type_counts);
        if (dominant == nullptr || !(*dominant)) {
            continue;
        }

        for (const VarKey& key : component) {
            auto occ_it = occurrences.find(key);
            if (occ_it == occurrences.end()) {
                continue;
            }
            for (Variable* var : occ_it->second) {
                if (var != nullptr) {
                    var->set_ir_type(*dominant);
                }
            }
        }
    }
}
void BitFieldComparisonUnrollingStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* branch = dynamic_cast<Branch*>(inst)) {
                (void)try_unroll_bitfield_branch_condition(task, branch);
            }
        }
    }
}
void ExpressionSimplificationStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            simplify_instruction_constants(task, inst);
        }
    }
}

void RedundantCastsEliminationStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            simplify_instruction_casts(task, inst);
        }
    }
}

void ArrayAccessDetectionStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            annotate_array_access_inst(inst);
        }
    }
}

void DeadComponentPrunerStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    ExpressionGraph graph = build_expression_graph(task.cfg());
    if (graph.sinks.empty()) return;

    const std::unordered_set<Instruction*> live = compute_live_component(graph);
    for (BasicBlock* block : task.cfg()->blocks()) {
        std::vector<Instruction*> kept;
        kept.reserve(block->instructions().size());
        for (Instruction* inst : block->instructions()) {
            if (live.contains(inst)) {
                kept.push_back(inst);
            }
        }
        if (kept.size() != block->instructions().size()) {
            block->set_instructions(std::move(kept));
        }
    }
}

void CommonSubexpressionEliminationStage::execute(DecompilerTask& task) {
    if (!task.cfg() || !task.cfg()->entry_block()) return;
    DominatorTree dom(*task.cfg());
    ExistingSubexpressionReplacer replacer(*task.cfg(), dom);
    replacer.run();

    DefinitionGenerator generator(task, *task.cfg(), dom);
    generator.run();
}

namespace {

struct EdgePrunerCandidate {
    std::string fingerprint;
    Expression* exemplar = nullptr;
    std::size_t complexity = 0;
    std::vector<std::size_t> instruction_indices;
};

bool edge_replace_in_expression(Expression*& expr, const std::string& fingerprint, Variable* replacement) {
    if (!expr) return false;

    if (expression_fingerprint(expr) == fingerprint) {
        expr = replacement;
        return true;
    }

    bool changed = false;
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        for (Expression*& child : op->mutable_operands()) {
            changed = edge_replace_in_expression(child, fingerprint, replacement) || changed;
        }
    } else if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression*& child : list->mutable_operands()) {
            changed = edge_replace_in_expression(child, fingerprint, replacement) || changed;
        }
    }
    return changed;
}

bool edge_rewrite_instruction(Instruction* inst, const std::string& fingerprint, Variable* replacement) {
    if (!inst) return false;

    bool changed = false;
    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
        Expression* value = assign->value();
        if (edge_replace_in_expression(value, fingerprint, replacement)) {
            assign->set_value(value);
            changed = true;
        }

        if (assign->destination() && !dynamic_cast<Variable*>(assign->destination())) {
            Expression* dest = assign->destination();
            if (edge_replace_in_expression(dest, fingerprint, replacement)) {
                assign->set_destination(dest);
                changed = true;
            }
        }
        return changed;
    }

    if (auto* branch = dynamic_cast<Branch*>(inst)) {
        if (auto* cond = branch->condition()) {
            for (Expression*& child : cond->mutable_operands()) {
                changed = edge_replace_in_expression(child, fingerprint, replacement) || changed;
            }
        }
        return changed;
    }

    if (auto* ib = dynamic_cast<IndirectBranch*>(inst)) {
        Expression* before = ib->expression();
        Expression* after = before;
        if (edge_replace_in_expression(after, fingerprint, replacement)) {
            if (after != before) {
                ib->substitute(before, after);
            }
            changed = true;
        }
        return changed;
    }

    if (auto* ret = dynamic_cast<Return*>(inst)) {
        for (Expression* value : ret->values()) {
            Expression* rewritten = value;
            if (edge_replace_in_expression(rewritten, fingerprint, replacement)) {
                if (rewritten != value) {
                    ret->substitute(value, rewritten);
                }
                changed = true;
            }
        }
        return changed;
    }

    return false;
}

std::optional<EdgePrunerCandidate> select_edge_pruner_candidate(BasicBlock* block) {
    if (!block) return std::nullopt;

    std::unordered_map<std::string, EdgePrunerCandidate> candidates;

    const auto& insts = block->instructions();
    for (std::size_t idx = 0; idx < insts.size(); ++idx) {
        Instruction* inst = insts[idx];
        if (!inst || dynamic_cast<Phi*>(inst)) continue;

        std::vector<Expression*> subexprs = instruction_subexpressions(inst);
        for (Expression* expr : subexprs) {
            if (!expr) continue;
            if (dynamic_cast<Variable*>(expr) || dynamic_cast<Constant*>(expr) || dynamic_cast<ListOperation*>(expr)) {
                continue;
            }
            if (contains_dereference(expr) || contains_call_expression(expr)) {
                continue;
            }

            const std::size_t complexity = expression_complexity(expr);
            if (complexity < 2) continue;

            const std::string fp = expression_fingerprint(expr);
            auto& candidate = candidates[fp];
            candidate.fingerprint = fp;
            candidate.exemplar = expr;
            candidate.complexity = complexity;
            candidate.instruction_indices.push_back(idx);
        }
    }

    std::optional<EdgePrunerCandidate> best;
    std::size_t best_score = 0;
    constexpr std::size_t kScoreThreshold = 6; // complexity * occurrences

    for (auto& [_, candidate] : candidates) {
        const std::size_t occurrences = candidate.instruction_indices.size();
        if (occurrences < 2) continue;
        const std::size_t score = candidate.complexity * occurrences;
        if (score < kScoreThreshold) continue;

        if (!best || score > best_score ||
            (score == best_score && candidate.complexity > best->complexity)) {
            best = candidate;
            best_score = score;
        }
    }

    return best;
}

bool apply_edge_pruner_candidate(
    DecompilerTask& task,
    BasicBlock* block,
    const EdgePrunerCandidate& candidate,
    int& temp_counter) {
    if (!block || !candidate.exemplar || candidate.instruction_indices.empty()) {
        return false;
    }

    auto insts = block->instructions();
    const std::size_t insert_idx = *std::min_element(
        candidate.instruction_indices.begin(), candidate.instruction_indices.end());

    const std::size_t width = candidate.exemplar->size_bytes > 0 ? candidate.exemplar->size_bytes : 4;
    auto* temp = task.arena().create<Variable>("edge_" + std::to_string(temp_counter++), width);
    temp->set_ssa_version(0);
    temp->set_ir_type(candidate.exemplar->ir_type());

    auto* def_inst = task.arena().create<Assignment>(temp, candidate.exemplar->copy(task.arena()));
    insts.insert(insts.begin() + static_cast<std::ptrdiff_t>(insert_idx), def_inst);

    bool any_rewrite = false;
    for (std::size_t i = insert_idx + 1; i < insts.size(); ++i) {
        any_rewrite = edge_rewrite_instruction(insts[i], candidate.fingerprint, temp) || any_rewrite;
    }

    if (!any_rewrite) {
        return false;
    }

    block->set_instructions(std::move(insts));
    return true;
}

} // namespace

void EdgePrunerStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    int temp_counter = 0;
    for (BasicBlock* block : task.cfg()->blocks()) {
        bool changed = true;
        while (changed) {
            changed = false;
            auto candidate = select_edge_pruner_candidate(block);
            if (!candidate.has_value()) {
                break;
            }
            changed = apply_edge_pruner_candidate(task, block, *candidate, temp_counter);
        }
    }
}

} // namespace aletheia
