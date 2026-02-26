#include "optimization_stages.hpp"
#include "../../dewolf_logic/z3_logic.hpp"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace dewolf {

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
// ExpressionPropagationStage -- Inter-block iterative fixed-point
// =============================================================================

void ExpressionPropagationStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    // Fixed-point outer loop
    for (int iteration = 0; iteration < 100; ++iteration) {
        // Step 1: Remove redundant phis before each iteration
        remove_redundant_phis(task.cfg(), task.arena());

        // Step 2: Build DefMap globally (Variable -> Assignment*)
        // In SSA form, each variable has exactly one definition.
        using DefMap = std::unordered_map<VarKey, Assignment*, VarKeyHash>;
        DefMap def_map;

        for (BasicBlock* block : task.cfg()->blocks()) {
            for (Instruction* inst : block->instructions()) {
                auto* assign = dynamic_cast<Assignment*>(inst);
                if (!assign) continue;
                // Phi is a subclass of Assignment, so this handles both
                if (auto* dest = dynamic_cast<Variable*>(assign->destination())) {
                    VarKey key = var_key(dest);
                    // In valid SSA, each variable is defined once.
                    // If we see a duplicate, keep the first (the later one may be
                    // from a previous propagation round creating a new assignment).
                    if (!def_map.contains(key)) {
                        def_map[key] = assign;
                    }
                }
            }
        }

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
                    if (!definition_can_be_propagated(def, inst)) continue;

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

void DeadPathEliminationStage::execute(DecompilerTask& task) {
    if (!task.cfg() || !task.cfg()->entry_block()) return;

    z3::context ctx;
    dewolf_logic::Z3Converter converter(ctx);

    std::unordered_set<Edge*> dead_edges;

    for (BasicBlock* block : task.cfg()->blocks()) {
        if (block->successors().size() > 1) {
            if (block->instructions().empty()) continue;
            Instruction* last_inst = block->instructions().back();
            if (auto* branch = dynamic_cast<Branch*>(last_inst)) {
                // If the branch is a generic Branch (not indirect), check its condition
                dewolf_logic::LogicCondition cond = converter.convert_to_condition(branch->condition());
                
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

    z3::context ctx;
    dewolf_logic::Z3Converter converter(ctx);

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
                    dewolf_logic::LogicCondition cond = converter.convert_to_condition(patched_cond);
                    
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
void TypePropagationStage::execute(DecompilerTask& task) {}
void BitFieldComparisonUnrollingStage::execute(DecompilerTask& task) {}
void CommonSubexpressionEliminationStage::execute(DecompilerTask& task) {}

} // namespace dewolf
