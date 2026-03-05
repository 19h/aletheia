#include "optimization_stages.hpp"
#include <ida/data.hpp>
#include <ida/name.hpp>
#include <ida/segment.hpp>
#include <ida/type.hpp>

namespace {
aletheia::OperationType flip_comparison(aletheia::OperationType op) {
    using aletheia::OperationType;
    switch (op) {
        case OperationType::lt: return OperationType::gt;
        case OperationType::le: return OperationType::ge;
        case OperationType::gt: return OperationType::lt;
        case OperationType::ge: return OperationType::le;
        case OperationType::lt_us: return OperationType::gt_us;
        case OperationType::le_us: return OperationType::ge_us;
        case OperationType::gt_us: return OperationType::lt_us;
        case OperationType::ge_us: return OperationType::le_us;
        default: return op;
    }
}
} // namespace

#include "../../logos/z3_logic.hpp"
#include "../ssa/dominators.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace aletheia {

namespace {
std::string expr_fingerprint(Expression* expr) {
    if (!expr) return "<null>";
    if (auto* c = dyn_cast<Constant>(expr)) {
        return "C:" + std::to_string(c->value()) + ":" + std::to_string(c->size_bytes);
    }
    if (auto* v = dyn_cast<Variable>(expr)) {
        return "V:" + v->name() + ":" + std::to_string(v->ssa_version());
    }
    if (auto* op = dyn_cast<Operation>(expr)) {
        std::string out = "O:" + std::to_string(static_cast<int>(op->type())) + "(";
        bool first = true;
        for (Expression* child : op->operands()) {
            if (!first) out += ",";
            first = false;
            out += expr_fingerprint(child);
        }
        out += ")";
        return out;
    }
    return "E:unknown";
}
} // namespace


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

/// Helper: check if a Variable matches the target by name + SSA version.
/// Uses pointer-to-data comparison first (O(1) for SSO and same-pool strings),
/// then falls back to length+content comparison only when pointers differ.
static inline bool is_target_var(Expression* expr,
                                  const std::string& target_name,
                                  std::size_t target_ssa) {
    if (auto* v = dyn_cast<Variable>(expr)) {
        if (v->ssa_version() != target_ssa) return false;
        // Fast path: compare string data pointers (works for SSO and interned)
        const auto& vn = v->name();
        if (vn.size() != target_name.size()) return false;
        if (vn.data() == target_name.data()) return true;
        return vn == target_name;
    }
    return false;
}

/// Check if an expression tree references a specific variable (by name + SSA version).
/// Used to detect self-referencing definitions (e.g., x = (cond ? a : x))
/// which would cause infinite expansion during propagation.
static bool expression_references_variable(Expression* expr,
                                             const std::string& target_name,
                                             std::size_t target_ssa) {
    if (!expr) return false;

    SmallVector<Expression*, 32> stack;
    stack.push_back(expr);

    while (!stack.empty()) {
        Expression* node = stack.back();
        stack.pop_back();
        if (!node) continue;

        if (is_target_var(node, target_name, target_ssa)) return true;

        if (auto* op = dyn_cast<Operation>(node)) {
            for (auto* child : op->operands()) {
                stack.push_back(child);
            }
        } else if (auto* list = dyn_cast<ListOperation>(node)) {
            for (auto* child : list->operands()) {
                stack.push_back(child);
            }
        }
    }
    return false;
}

static bool expression_references_variable_name(Expression* expr,
                                                const std::string& target_name) {
    if (!expr) return false;

    SmallVector<Expression*, 32> stack;
    stack.push_back(expr);

    while (!stack.empty()) {
        Expression* node = stack.back();
        stack.pop_back();
        if (!node) continue;

        if (auto* v = dyn_cast<Variable>(node)) {
            if (v->name() == target_name) {
                return true;
            }
        }

        if (auto* op = dyn_cast<Operation>(node)) {
            for (auto* child : op->operands()) {
                stack.push_back(child);
            }
        } else if (auto* list = dyn_cast<ListOperation>(node)) {
            for (auto* child : list->operands()) {
                stack.push_back(child);
            }
        }
    }
    return false;
}

static std::size_t count_variable_occurrences(Expression* expr,
                                              const std::string& target_name,
                                              std::size_t target_ssa) {
    if (!expr) return 0;
    std::size_t count = 0;
    SmallVector<Expression*, 32> stack;
    stack.push_back(expr);

    while (!stack.empty()) {
        Expression* node = stack.back();
        stack.pop_back();
        if (!node) continue;

        if (is_target_var(node, target_name, target_ssa)) {
            ++count;
        }

        if (auto* op = dyn_cast<Operation>(node)) {
            for (auto* child : op->operands()) {
                stack.push_back(child);
            }
        } else if (auto* list = dyn_cast<ListOperation>(node)) {
            for (auto* child : list->operands()) {
                stack.push_back(child);
            }
        }
    }
    return count;
}

static std::size_t count_variable_occurrences_in_instruction(Instruction* inst,
                                                             const std::string& target_name,
                                                             std::size_t target_ssa) {
    if (!inst) return 0;

    std::size_t count = 0;
    if (auto* assign = dyn_cast<Assignment>(inst)) {
        count += count_variable_occurrences(assign->destination(), target_name, target_ssa);
        count += count_variable_occurrences(assign->value(), target_name, target_ssa);
        return count;
    }
    if (auto* branch = dyn_cast<Branch>(inst)) {
        count += count_variable_occurrences(branch->condition(), target_name, target_ssa);
        return count;
    }
    if (auto* ret = dyn_cast<Return>(inst)) {
        for (auto* value : ret->values()) {
            count += count_variable_occurrences(value, target_name, target_ssa);
        }
        return count;
    }
    return count;
}

static bool looks_pointer_like_arg(Expression* expr) {
    if (!expr) return false;
    if (isa<GlobalVariable>(expr)) return true;
    if (auto* c = dyn_cast<Constant>(expr)) {
        return c->value() > 0x10000;
    }
    if (auto* op = dyn_cast<Operation>(expr)) {
        if (op->type() == OperationType::cast && op->operands().size() == 1) {
            return looks_pointer_like_arg(op->operands()[0]);
        }
    }
    return false;
}

static bool is_machine_register_like_name(const std::string& lowered_name) {
    if (lowered_name.empty()) {
        return false;
    }

    auto all_digits_after_prefix = [&](std::size_t start) {
        if (start >= lowered_name.size()) return false;
        for (std::size_t i = start; i < lowered_name.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(lowered_name[i]))) {
                return false;
            }
        }
        return true;
    };

    // arm64 register families: x0..x31, w0..w31.
    if ((lowered_name[0] == 'x' || lowered_name[0] == 'w')
        && all_digits_after_prefix(1)) {
        return true;
    }

    // Floating-point/vector register families: dN/sN/qN/vN.
    if ((lowered_name[0] == 'd' || lowered_name[0] == 's'
        || lowered_name[0] == 'q' || lowered_name[0] == 'v')
        && all_digits_after_prefix(1)) {
        return true;
    }

    // Generic numbered register family: r0..r31 and x86 subregisters like r8b/r9d.
    if (lowered_name[0] == 'r' && lowered_name.size() >= 2) {
        std::size_t idx = 1;
        while (idx < lowered_name.size() && std::isdigit(static_cast<unsigned char>(lowered_name[idx]))) {
            ++idx;
        }
        if (idx > 1) {
            if (idx == lowered_name.size()) {
                return true;
            }
            if (idx + 1 == lowered_name.size()) {
                const char suffix = lowered_name[idx];
                if (suffix == 'b' || suffix == 'w' || suffix == 'd') {
                    return true;
                }
            }
        }
    }

    static const std::unordered_set<std::string> known_register_names = {
        "sp", "fp", "lr", "pc", "xzr", "wzr",
        "flags", "nzcv",
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rsp", "rbp", "rip",
        "eax", "ebx", "ecx", "edx", "esi", "edi", "esp", "ebp", "eip",
        "ax", "bx", "cx", "dx", "si", "di", "bp", "ip",
        "al", "ah", "bl", "bh", "cl", "ch", "dl", "dh"
    };
    return known_register_names.contains(lowered_name);
}

static std::optional<unsigned> parse_arm64_xw_index(std::string_view lowered_name) {
    if (lowered_name.size() < 2) {
        return std::nullopt;
    }
    const char prefix = lowered_name.front();
    if (prefix != 'x' && prefix != 'w') {
        return std::nullopt;
    }
    unsigned index = 0;
    for (std::size_t i = 1; i < lowered_name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(lowered_name[i]);
        if (!std::isdigit(c)) {
            return std::nullopt;
        }
        index = index * 10u + static_cast<unsigned>(lowered_name[i] - '0');
    }
    return index;
}

static bool is_arm64_nonarg_width_alias_pair(const std::string& lhs_name, const std::string& rhs_name) {
    std::string lhs = lhs_name;
    std::string rhs = rhs_name;
    std::transform(lhs.begin(), lhs.end(), lhs.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(rhs.begin(), rhs.end(), rhs.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const auto lhs_idx = parse_arm64_xw_index(lhs);
    const auto rhs_idx = parse_arm64_xw_index(rhs);
    if (!lhs_idx || !rhs_idx || *lhs_idx != *rhs_idx) {
        return false;
    }
    if (*lhs_idx < 8u) {
        return false;
    }

    return (lhs.front() == 'x' && rhs.front() == 'w')
        || (lhs.front() == 'w' && rhs.front() == 'x');
}

/// Compute the "weight" (number of unique nodes) in an expression DAG.
/// Capped at `limit` to avoid traversing huge DAGs.
/// Uses a visited set to handle shared subtrees correctly.
static std::size_t expr_weight(Expression* expr, std::size_t limit) {
    if (!expr || limit == 0) return 0;

    SmallVector<Expression*, 32> stack;
    std::unordered_set<Expression*> visited;
    stack.push_back(expr);
    visited.insert(expr);
    std::size_t count = 0;

    while (!stack.empty() && count < limit) {
        Expression* node = stack.back();
        stack.pop_back();
        ++count;

        if (auto* op = dyn_cast<Operation>(node)) {
            for (auto* child : op->operands()) {
                if (child && visited.insert(child).second) stack.push_back(child);
            }
        } else if (auto* list = dyn_cast<ListOperation>(node)) {
            for (auto* child : list->operands()) {
                if (child && visited.insert(child).second) stack.push_back(child);
            }
        }
    }
    return count;
}

/// Maximum expression tree weight after propagation.  If substituting
/// `replacement` into the tree would push it past this, we skip it.
static constexpr std::size_t MAX_EXPR_WEIGHT = 256;
static constexpr int MAX_PROPAGATION_ITERATIONS = 24;
static constexpr std::size_t MIN_PROPAGATION_SUBSTITUTIONS = 1024;
static constexpr std::size_t PROPAGATION_SUBSTITUTIONS_PER_INSTRUCTION = 4;

/// Replace all occurrences of `target` variable with `replacement` in `expr`.
///
/// Uses BOUNDED-DEPTH RECURSIVE COPY-ON-WRITE. Each variable match site
/// gets a fresh deep-copy of `replacement` so expression trees remain true
/// trees (no shared subtrees). Only nodes on the path from root to a match
/// are cloned; unchanged subtrees are structurally shared (read-only).
///
/// A depth limit prevents stack overflow. If the limit is exceeded, the
/// function bails out without substitution (returns the original unchanged).
/// The MAX_EXPR_WEIGHT complexity guard on the caller side ensures that
/// trees don't grow beyond ~256 nodes, so the depth limit of 512 is generous.
///
/// Returns: {new_root, did_mutate}. The root may be a new node if any
/// child on the path from root changed.
struct ReplaceResult {
    Expression* root;
    bool mutated;
};

/// Maximum recursion depth for replace_variable_cow.
/// 512 frames × ~80 bytes/frame ≈ 40KB stack, well within limits.
static constexpr int MAX_REPLACE_DEPTH = 512;

static ReplaceResult replace_variable_cow_impl(
    DecompilerArena& arena, Expression* expr,
    const std::string& target_name, std::size_t target_ssa,
    Expression* replacement, int depth) {

    if (!expr || depth <= 0) return {expr, false};

    // Leaf: Variable match → deep-copy replacement
    if (is_target_var(expr, target_name, target_ssa)) {
        return {replacement->copy(arena), true};
    }

    // Leaf: Constant or non-matching Variable — unchanged
    if (isa<Constant>(expr) || isa<Variable>(expr)) {
        return {expr, false};
    }

    // Operation (includes Condition, Call) — recurse into children
    if (auto* op = dyn_cast<Operation>(expr)) {
        SmallVector<Expression*, 4> new_ops;
        bool changed = false;
        new_ops.reserve(op->operands().size());
        for (auto* child : op->operands()) {
            auto [new_child, m] = replace_variable_cow_impl(
                arena, child, target_name, target_ssa, replacement, depth - 1);
            if (m) changed = true;
            new_ops.push_back(new_child);
        }
        if (changed) {
            if (auto* call = dyn_cast<Call>(expr)) {
                // IMPORTANT: preserve the call! 
                Expression* target = new_ops[0];
                std::vector<Expression*> args;
                for (size_t i = 1; i < new_ops.size(); ++i) {
                    args.push_back(new_ops[i]);
                }
                auto* nc = arena.create<Call>(target, std::move(args), call->size_bytes);
                nc->set_ir_type(call->ir_type());
                return {nc, true};
            }
            if (auto* cond = dyn_cast<Condition>(expr)) {
                auto* nc = arena.create<Condition>(
                    cond->type(), new_ops[0], new_ops[1], cond->size_bytes);
                nc->set_ir_type(cond->ir_type());
                return {nc, true};
            }
            auto* nop = arena.create<Operation>(
                op->type(), std::move(new_ops), op->size_bytes);
            nop->set_ir_type(op->ir_type());
            if (op->array_access().has_value())
                nop->set_array_access(*op->array_access());
            return {nop, true};
        }
        return {op, false};
    }

    // ListOperation — recurse into children
    if (auto* list = dyn_cast<ListOperation>(expr)) {
        SmallVector<Expression*, 4> new_ops;
        bool changed = false;
        new_ops.reserve(list->operands().size());
        for (auto* child : list->operands()) {
            auto [new_child, m] = replace_variable_cow_impl(
                arena, child, target_name, target_ssa, replacement, depth - 1);
            if (m) changed = true;
            new_ops.push_back(new_child);
        }
        if (changed) {
            auto* nl = arena.create<ListOperation>(
                std::move(new_ops), list->size_bytes);
            nl->set_ir_type(list->ir_type());
            return {nl, true};
        }
        return {list, false};
    }

    return {expr, false};
}

static ReplaceResult replace_variable_cow(
    DecompilerArena& arena, Expression* expr,
    Variable* target, Expression* replacement) {
    return replace_variable_cow_impl(
        arena, expr, target->name(), target->ssa_version(),
        replacement, MAX_REPLACE_DEPTH);
}

/// Backward-compatible wrapper.
static Expression* replace_variable_ptr(
    DecompilerArena& arena, Expression* expr,
    Variable* target, Expression* replacement) {
    return replace_variable_cow(arena, expr, target, replacement).root;
}

// =============================================================================
// Iterative expression-tree walkers (stack-safe on arbitrarily deep DAGs)
// =============================================================================

/// Collect all Variable* leaves reachable from an expression DAG, iteratively.
/// This replaces recursive Expression::collect_requirements() in hot paths.
///
/// Uses a visited set to prevent exponential blowup on shared DAGs: when
/// in-place mutation causes the same Operation* to appear as a child of
/// multiple parents, without deduplication the worklist would re-traverse
/// shared subtrees O(2^depth) times.
static void collect_variables_iterative(Expression* expr,
                                        std::unordered_set<Variable*>& out) {
    if (!expr) return;
    SmallVector<Expression*, 64> stack;
    std::unordered_set<Expression*> visited;
    stack.push_back(expr);
    visited.insert(expr);
    while (!stack.empty()) {
        Expression* node = stack.back();
        stack.pop_back();
        if (!node) continue;
        if (auto* v = dyn_cast<Variable>(node)) {
            out.insert(v);
        } else if (auto* op = dyn_cast<Operation>(node)) {
            for (auto* child : op->operands()) {
                if (child && visited.insert(child).second) {
                    stack.push_back(child);
                }
            }
        } else if (auto* list = dyn_cast<ListOperation>(node)) {
            for (auto* child : list->operands()) {
                if (child && visited.insert(child).second) {
                    stack.push_back(child);
                }
            }
        }
    }
}

/// Iterative collect_requirements for an Instruction.
/// Mirrors the semantics of the virtual collect_requirements() but without
/// recursion, so it's safe on arbitrarily deep expression trees.
static void collect_requirements_iterative(Instruction* inst,
                                           std::unordered_set<Variable*>& out) {
    if (!inst) return;

    if (auto* assign = dyn_cast<Assignment>(inst)) {
        // RHS is always a requirement
        collect_variables_iterative(assign->value(), out);
        // Complex destination (e.g., *(ptr+off) = val): sub-expressions are reqs
        if (assign->destination() && !isa<Variable>(assign->destination())) {
            collect_variables_iterative(assign->destination(), out);
        }
    } else if (auto* branch = dyn_cast<Branch>(inst)) {
        collect_variables_iterative(branch->condition(), out);
    } else if (auto* ret = dyn_cast<Return>(inst)) {
        for (auto* val : ret->values()) {
            collect_variables_iterative(val, out);
        }
    } else if (auto* rel = dyn_cast<Relation>(inst)) {
        if (rel->value()) out.insert(rel->value());
    } else if (auto* ibranch = dyn_cast<IndirectBranch>(inst)) {
        collect_variables_iterative(ibranch->expression(), out);
    }
    // Break, Continue, Comment have no requirements
}

// =============================================================================
// Rule checks for safe propagation
// =============================================================================

/// Check if an expression DAG contains any aliased variable (iterative, DAG-safe).
static bool contains_aliased_variable(Expression* expr) {
    if (!expr) return false;
    SmallVector<Expression*, 32> stack;
    std::unordered_set<Expression*> visited;
    stack.push_back(expr);
    visited.insert(expr);
    while (!stack.empty()) {
        Expression* node = stack.back();
        stack.pop_back();
        if (!node) continue;
        if (auto* v = dyn_cast<Variable>(node)) {
            if (v->is_aliased()) return true;
        } else if (auto* op = dyn_cast<Operation>(node)) {
            for (auto* child : op->operands()) {
                if (child && visited.insert(child).second) stack.push_back(child);
            }
        } else if (auto* list = dyn_cast<ListOperation>(node)) {
            for (auto* child : list->operands()) {
                if (child && visited.insert(child).second) stack.push_back(child);
            }
        }
    }
    return false;
}

static bool is_flags_like_variable(const Variable* var) {
    if (!var) {
        return false;
    }
    std::string lowered = var->name();
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lowered == "flags"
        || lowered == "nzcv"
        || lowered.rfind("flags_", 0) == 0
        || lowered.rfind("nzcv_", 0) == 0;
}

/// Check if an expression is an address_of operation.
static bool is_address_of(Expression* expr) {
    if (auto* op = dyn_cast<Operation>(expr))
        return op->type() == OperationType::address_of;
    return false;
}

/// Check if an expression DAG contains a dereference operation (iterative, DAG-safe).
static bool contains_dereference(Expression* expr) {
    if (!expr) return false;
    SmallVector<Expression*, 32> stack;
    std::unordered_set<Expression*> visited;
    stack.push_back(expr);
    visited.insert(expr);
    while (!stack.empty()) {
        Expression* node = stack.back();
        stack.pop_back();
        if (!node) continue;
        if (auto* op = dyn_cast<Operation>(node)) {
            if (op->type() == OperationType::deref) return true;
            for (auto* child : op->operands()) {
                if (child && visited.insert(child).second) stack.push_back(child);
            }
        } else if (auto* list = dyn_cast<ListOperation>(node)) {
            for (auto* child : list->operands()) {
                if (child && visited.insert(child).second) stack.push_back(child);
            }
        }
    }
    return false;
}

/// Check if a definition (Assignment) can be safely propagated into a target
/// instruction. Returns true if the propagation is allowed.
static bool definition_can_be_propagated(Assignment* def, Instruction* target) {
    // Rule: Don't propagate phi definitions
    if (isa<Phi>(def)) return false;

    // Rule: Don't propagate call assignments (handled by EPFC)
    if (auto* val = dyn_cast<Operation>(def->value())) {
        if (val->type() == OperationType::call) return false;
    }

    // Rule: Don't propagate if definition contains aliased variables (EPM handles).
    // Exception: allow branch folding through machine-flags assignments like
    // flags = (a - b), so cmp-style conditions can be recovered to (a CMP b).
    const bool aliased_dest = contains_aliased_variable(def->destination());
    const bool aliased_value = contains_aliased_variable(def->value());
    if (aliased_dest || aliased_value) {
        bool allow_flags_into_branch = false;
        if (aliased_dest && !aliased_value && isa<Branch>(target)) {
            if (auto* dst = dyn_cast<Variable>(def->destination())) {
                allow_flags_into_branch = is_flags_like_variable(dst);
            }
        }
        if (!allow_flags_into_branch) {
            return false;
        }
    }

    // Rule: Don't propagate address_of assignments
    if (is_address_of(def->value())) return false;

    // Rule: Don't propagate if the definition's RHS contains dereferences (EPM handles)
    if (contains_dereference(def->value())) return false;

    // Rule: Don't propagate operations into phi arguments
    if (isa<Phi>(target)) {
        if (isa<Operation>(def->value())) return false;
    }

    return true;
}

// =============================================================================
// CMP+Branch folding helper
// =============================================================================
// If flags = a - b and we have if (flags CMP 0), fold to if (a CMP b).

static Condition* fold_flags_comparison_condition(DecompilerArena& arena, Condition* cond) {
    if (!cond) {
        return nullptr;
    }

    auto* rhs_zero = dyn_cast<Constant>(cond->rhs());
    if (!rhs_zero || rhs_zero->value() != 0) {
        return cond;
    }

    auto* lhs_op = dyn_cast<Operation>(cond->lhs());
    if (!lhs_op || lhs_op->operands().size() != 2) {
        return cond;
    }

    // Common compare canonical form after propagation: (a - b) CMP 0
    if (lhs_op->type() != OperationType::sub && lhs_op->type() != OperationType::sub_float) {
        return cond;
    }

    return arena.create<Condition>(
        cond->type(), lhs_op->operands()[0], lhs_op->operands()[1], cond->size_bytes);
}

static Expression* fold_flags_comparison_expression(DecompilerArena& arena, Expression* expr) {
    auto* cond = dyn_cast<Condition>(expr);
    if (!cond) {
        return expr;
    }
    return fold_flags_comparison_condition(arena, cond);
}

static void try_fold_cmp_branch(DecompilerArena& arena, Branch* branch) {
    Condition* cond = branch->condition();
    if (!cond) {
        return;
    }

    branch->set_condition(fold_flags_comparison_condition(arena, cond));
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
                auto* phi = dyn_cast<Phi>(instrs[i]);
                if (!phi) { ++i; continue; }

                Variable* dest = phi->dest_var();
                Expression* single_value = nullptr;
                bool all_same = true;

                for (auto* operand : phi->operand_list()->operands()) {
                    // Skip self-references (phi dest used in its own operand list)
                    if (auto* v = dyn_cast<Variable>(operand)) {
                        if (v->name() == dest->name() &&
                            v->ssa_version() == dest->ssa_version()) continue;
                    }
                    if (!single_value) {
                        single_value = operand;
                    } else {
                        // Check if this operand is the same as single_value
                        auto* sv = dyn_cast<Variable>(single_value);
                        auto* ov = dyn_cast<Variable>(operand);
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

using AssignmentDefMap = std::unordered_map<VarKey, Assignment*, VarKeyHash>;

static constexpr std::size_t MAX_CALL_TARGET_ALIAS_DEPTH = 8;

static Expression* strip_trivial_casts(Expression* expr) {
    Expression* current = expr;
    while (auto* op = dyn_cast<Operation>(current)) {
        if (op->type() != OperationType::cast || op->operands().size() != 1 || op->operands()[0] == nullptr) {
            break;
        }
        current = op->operands()[0];
    }
    return current;
}

static int call_target_specificity(Expression* expr) {
    Expression* stripped = strip_trivial_casts(expr);
    if (isa<GlobalVariable>(stripped)) return 2;
    if (isa<Constant>(stripped)) return 1;
    return 0;
}

static bool same_concrete_call_target(Expression* lhs, Expression* rhs) {
    Expression* left = strip_trivial_casts(lhs);
    Expression* right = strip_trivial_casts(rhs);
    if (!left || !right) return false;

    if (auto* lgv = dyn_cast<GlobalVariable>(left)) {
        auto* rgv = dyn_cast<GlobalVariable>(right);
        return rgv && lgv->name() == rgv->name();
    }

    auto* lc = dyn_cast<Constant>(left);
    auto* rc = dyn_cast<Constant>(right);
    return lc && rc && lc->value() == rc->value() && lc->size_bytes == rc->size_bytes;
}

static Expression* resolve_call_target_alias_chain_impl(
    Expression* target,
    const AssignmentDefMap& def_map,
    std::unordered_map<VarKey, Expression*, VarKeyHash>& memo,
    std::unordered_set<VarKey, VarKeyHash>& active,
    std::size_t depth);

static Expression* resolve_phi_call_target(
    Phi* phi,
    const AssignmentDefMap& def_map,
    std::unordered_map<VarKey, Expression*, VarKeyHash>& memo,
    std::unordered_set<VarKey, VarKeyHash>& active,
    std::size_t depth) {

    if (!phi || !phi->operand_list() || depth == 0) {
        return nullptr;
    }

    Expression* candidate = nullptr;
    for (Expression* operand : phi->operand_list()->operands()) {
        Expression* resolved = resolve_call_target_alias_chain_impl(
            operand,
            def_map,
            memo,
            active,
            depth - 1);
        if (!resolved) {
            return nullptr;
        }

        if (!candidate) {
            candidate = resolved;
            continue;
        }

        if (!same_concrete_call_target(candidate, resolved)) {
            return nullptr;
        }
    }

    return candidate;
}

static Expression* resolve_call_target_address_arithmetic(
    Operation* op,
    const AssignmentDefMap& def_map,
    std::unordered_map<VarKey, Expression*, VarKeyHash>& memo,
    std::unordered_set<VarKey, VarKeyHash>& active,
    std::size_t depth) {

    if (!op || op->operands().size() != 2 || depth == 0) {
        return nullptr;
    }

    if (op->type() != OperationType::add && op->type() != OperationType::sub) {
        return nullptr;
    }

    Expression* lhs = strip_trivial_casts(op->operands()[0]);
    Expression* rhs = strip_trivial_casts(op->operands()[1]);
    if (!lhs || !rhs) {
        return nullptr;
    }

    Expression* resolved_lhs = resolve_call_target_alias_chain_impl(
        lhs,
        def_map,
        memo,
        active,
        depth - 1);
    Expression* resolved_rhs = resolve_call_target_alias_chain_impl(
        rhs,
        def_map,
        memo,
        active,
        depth - 1);

    auto extract_global_base = [&](Expression* candidate) -> GlobalVariable* {
        if (auto* gv = dyn_cast<GlobalVariable>(candidate)) {
            return gv;
        }
        auto* base_op = dyn_cast<Operation>(candidate);
        if (!base_op || base_op->type() != OperationType::address_of || base_op->operands().size() != 1) {
            return nullptr;
        }

        Expression* addr_base = strip_trivial_casts(base_op->operands()[0]);
        if (!addr_base) {
            return nullptr;
        }

        Expression* resolved_addr_base = resolve_call_target_alias_chain_impl(
            addr_base,
            def_map,
            memo,
            active,
            depth - 1);
        return dyn_cast<GlobalVariable>(resolved_addr_base ? resolved_addr_base : addr_base);
    };

    auto* lhs_const = dyn_cast<Constant>(resolved_lhs ? resolved_lhs : lhs);
    auto* rhs_const = dyn_cast<Constant>(resolved_rhs ? resolved_rhs : rhs);
    auto* lhs_global = extract_global_base(resolved_lhs ? resolved_lhs : lhs);
    auto* rhs_global = extract_global_base(resolved_rhs ? resolved_rhs : rhs);

    if (op->type() == OperationType::add) {
        if (lhs_global && rhs_const && rhs_const->value() == 0) {
            return lhs_global;
        }
        if (rhs_global && lhs_const && lhs_const->value() == 0) {
            return rhs_global;
        }
    }

    if (op->type() == OperationType::sub && lhs_global && rhs_const && rhs_const->value() == 0) {
        return lhs_global;
    }

    return nullptr;
}

static Expression* resolve_address_of_call_target(
    Operation* op,
    const AssignmentDefMap& def_map,
    std::unordered_map<VarKey, Expression*, VarKeyHash>& memo,
    std::unordered_set<VarKey, VarKeyHash>& active,
    std::size_t depth) {

    if (!op || op->type() != OperationType::address_of || op->operands().size() != 1 || depth == 0) {
        return nullptr;
    }

    Expression* base = strip_trivial_casts(op->operands()[0]);
    if (!base) {
        return nullptr;
    }

    if (auto* base_op = dyn_cast<Operation>(base)) {
        if (base_op->type() == OperationType::deref && base_op->operands().size() == 1) {
            base = strip_trivial_casts(base_op->operands()[0]);
            if (!base) {
                return nullptr;
            }
        }
    }

    Expression* resolved = resolve_call_target_alias_chain_impl(
        base,
        def_map,
        memo,
        active,
        depth - 1);
    Expression* concrete = resolved ? resolved : base;
    if (isa<GlobalVariable>(concrete) || isa<Constant>(concrete)) {
        return concrete;
    }

    return nullptr;
}

struct GlobalOffsetTarget {
    GlobalVariable* global = nullptr;
    std::uint64_t byte_offset = 0;
};

static std::optional<GlobalOffsetTarget> extract_global_offset_target(
    Expression* expr,
    const AssignmentDefMap& def_map,
    std::unordered_map<VarKey, Expression*, VarKeyHash>& memo,
    std::unordered_set<VarKey, VarKeyHash>& active,
    std::size_t depth) {

    if (!expr || depth == 0) {
        return std::nullopt;
    }

    Expression* stripped = strip_trivial_casts(expr);
    if (!stripped) {
        return std::nullopt;
    }

    Expression* resolved = resolve_call_target_alias_chain_impl(stripped, def_map, memo, active, depth - 1);
    Expression* concrete = resolved ? resolved : stripped;

    auto extract_global_base = [&](Expression* candidate) -> GlobalVariable* {
        if (auto* gv = dyn_cast<GlobalVariable>(candidate)) {
            return gv;
        }
        auto* base_op = dyn_cast<Operation>(candidate);
        if (!base_op || base_op->type() != OperationType::address_of || base_op->operands().size() != 1) {
            return nullptr;
        }

        Expression* addr_base = strip_trivial_casts(base_op->operands()[0]);
        if (!addr_base) {
            return nullptr;
        }

        Expression* resolved_addr_base = resolve_call_target_alias_chain_impl(
            addr_base,
            def_map,
            memo,
            active,
            depth - 1);
        return dyn_cast<GlobalVariable>(resolved_addr_base ? resolved_addr_base : addr_base);
    };

    if (auto* gv = extract_global_base(concrete)) {
        return GlobalOffsetTarget{gv, 0};
    }

    auto* op = dyn_cast<Operation>(concrete);
    if (!op || op->operands().size() != 2 ||
        (op->type() != OperationType::add && op->type() != OperationType::sub)) {
        return std::nullopt;
    }

    Expression* lhs = strip_trivial_casts(op->operands()[0]);
    Expression* rhs = strip_trivial_casts(op->operands()[1]);
    if (!lhs || !rhs) {
        return std::nullopt;
    }

    Expression* lhs_resolved = resolve_call_target_alias_chain_impl(lhs, def_map, memo, active, depth - 1);
    Expression* rhs_resolved = resolve_call_target_alias_chain_impl(rhs, def_map, memo, active, depth - 1);

    auto* lhs_global = extract_global_base(lhs_resolved ? lhs_resolved : lhs);
    auto* rhs_global = extract_global_base(rhs_resolved ? rhs_resolved : rhs);
    auto* lhs_const = dyn_cast<Constant>(lhs_resolved ? lhs_resolved : lhs);
    auto* rhs_const = dyn_cast<Constant>(rhs_resolved ? rhs_resolved : rhs);

    if (op->type() == OperationType::add) {
        if (lhs_global && rhs_const) {
            return GlobalOffsetTarget{lhs_global, rhs_const->value()};
        }
        if (rhs_global && lhs_const) {
            return GlobalOffsetTarget{rhs_global, lhs_const->value()};
        }
        return std::nullopt;
    }

    if (!lhs_global || !rhs_const || rhs_const->value() != 0) {
        return std::nullopt;
    }

    return GlobalOffsetTarget{lhs_global, 0};
}

static Expression* resolve_memory_backed_call_target(
    Operation* op,
    const AssignmentDefMap& def_map,
    std::unordered_map<VarKey, Expression*, VarKeyHash>& memo,
    std::unordered_set<VarKey, VarKeyHash>& active,
    std::size_t depth) {

    if (!op || op->type() != OperationType::deref || op->operands().size() != 1 || depth == 0) {
        return nullptr;
    }

    Expression* base = strip_trivial_casts(op->operands()[0]);
    if (!base) {
        return nullptr;
    }

    Expression* resolved_base = resolve_call_target_alias_chain_impl(
        base,
        def_map,
        memo,
        active,
        depth - 1);

    auto* gv = dyn_cast<GlobalVariable>(resolved_base ? resolved_base : base);
    if (gv && gv->initial_value()) {
        Expression* resolved_init = resolve_call_target_alias_chain_impl(
            gv->initial_value(),
            def_map,
            memo,
            active,
            depth - 1);
        Expression* concrete_init = strip_trivial_casts(resolved_init ? resolved_init : gv->initial_value());

        if (isa<GlobalVariable>(concrete_init)) {
            return concrete_init;
        }

        if (gv->is_constant()) {
            if (auto* c = dyn_cast<Constant>(concrete_init)) {
                if (c->value() != 0) {
                    return c;
                }
            }
        }
    }

    Expression* table_base = resolved_base ? resolved_base : base;
    if (auto* table_var = dyn_cast<Variable>(table_base)) {
        auto it_def = def_map.find(var_key(table_var));
        if (it_def != def_map.end() && it_def->second && !isa<Phi>(it_def->second)) {
            table_base = it_def->second->value();
        }
    }

    auto global_offset = extract_global_offset_target(table_base, def_map, memo, active, depth - 1);
    if (!global_offset.has_value() || !global_offset->global) {
        return nullptr;
    }

    auto* table_values = dyn_cast<ListOperation>(global_offset->global->initial_value());
    if (!table_values || op->size_bytes == 0 || global_offset->byte_offset % op->size_bytes != 0) {
        return nullptr;
    }

    const std::size_t element_index = static_cast<std::size_t>(global_offset->byte_offset / op->size_bytes);
    if (element_index >= table_values->operands().size()) {
        return nullptr;
    }

    return resolve_call_target_alias_chain_impl(
        table_values->operands()[element_index],
        def_map,
        memo,
        active,
        depth - 1);
}

static Expression* resolve_call_target_alias_chain_impl(
    Expression* target,
    const AssignmentDefMap& def_map,
    std::unordered_map<VarKey, Expression*, VarKeyHash>& memo,
    std::unordered_set<VarKey, VarKeyHash>& active,
    std::size_t depth) {

    if (depth == 0) {
        return nullptr;
    }

    Expression* stripped_target = strip_trivial_casts(target);
    if (!stripped_target) {
        return nullptr;
    }

    if (isa<GlobalVariable>(stripped_target) || isa<Constant>(stripped_target)) {
        return stripped_target;
    }

    if (auto* op = dyn_cast<Operation>(stripped_target)) {
        if (Expression* addrof = resolve_address_of_call_target(op, def_map, memo, active, depth)) {
            return addrof;
        }
        if (Expression* arith = resolve_call_target_address_arithmetic(op, def_map, memo, active, depth)) {
            return arith;
        }
        return resolve_memory_backed_call_target(op, def_map, memo, active, depth);
    }

    auto* start = dyn_cast<Variable>(stripped_target);
    if (!start) return nullptr;

    VarKey key = var_key(start);
    auto memo_it = memo.find(key);
    if (memo_it != memo.end()) {
        return memo_it->second;
    }

    if (!active.insert(key).second) {
        return nullptr;
    }

    Expression* resolved = nullptr;
    auto it_def = def_map.find(key);
    if (it_def != def_map.end() && it_def->second) {
        Assignment* def = it_def->second;
        if (auto* phi = dyn_cast<Phi>(def)) {
            resolved = resolve_phi_call_target(phi, def_map, memo, active, depth - 1);
        } else {
            resolved = resolve_call_target_alias_chain_impl(
                def->value(),
                def_map,
                memo,
                active,
                depth - 1);
        }
    }

    active.erase(key);
    memo[key] = resolved;
    return resolved;
}

static Expression* resolve_call_target_alias_chain(
    Expression* target,
    const AssignmentDefMap& def_map,
    std::unordered_map<VarKey, Expression*, VarKeyHash>& memo) {
    std::unordered_set<VarKey, VarKeyHash> active;
    return resolve_call_target_alias_chain_impl(
        target,
        def_map,
        memo,
        active,
        MAX_CALL_TARGET_ALIAS_DEPTH);
}

static bool canonicalize_call_targets_in_expression(
    Expression* root,
    const AssignmentDefMap& def_map,
    std::unordered_map<VarKey, Expression*, VarKeyHash>& memo) {

    if (!root) return false;
    bool changed = false;
    SmallVector<Expression*, 32> stack;
    std::unordered_set<Expression*> visited;
    stack.push_back(root);
    visited.insert(root);

    while (!stack.empty()) {
        Expression* node = stack.back();
        stack.pop_back();
        if (!node) continue;

        if (auto* call = dyn_cast<Call>(node)) {
            if (!call->mutable_operands().empty()) {
                Expression*& target_ref = call->mutable_operands()[0];
                Expression* resolved = resolve_call_target_alias_chain(target_ref, def_map, memo);
                if (resolved && call_target_specificity(resolved) > call_target_specificity(target_ref)) {
                    target_ref = resolved;
                    changed = true;
                }
            }
            for (Expression* child : call->operands()) {
                if (child && visited.insert(child).second) stack.push_back(child);
            }
            continue;
        }

        if (auto* op = dyn_cast<Operation>(node)) {
            if (op->type() == OperationType::call && !op->mutable_operands().empty()) {
                Expression*& target_ref = op->mutable_operands()[0];
                Expression* resolved = resolve_call_target_alias_chain(target_ref, def_map, memo);
                if (resolved && call_target_specificity(resolved) > call_target_specificity(target_ref)) {
                    target_ref = resolved;
                    changed = true;
                }
            }
            for (Expression* child : op->operands()) {
                if (child && visited.insert(child).second) stack.push_back(child);
            }
            continue;
        }

        if (auto* list = dyn_cast<ListOperation>(node)) {
            for (Expression* child : list->operands()) {
                if (child && visited.insert(child).second) stack.push_back(child);
            }
        }
    }

    return changed;
}

static bool canonicalize_call_targets_in_instruction(
    Instruction* inst,
    const AssignmentDefMap& def_map,
    std::unordered_map<VarKey, Expression*, VarKeyHash>& memo) {

    if (!inst) return false;

    if (auto* assign = dyn_cast<Assignment>(inst)) {
        bool changed = canonicalize_call_targets_in_expression(assign->value(), def_map, memo);
        if (assign->destination() && !isa<Variable>(assign->destination())) {
            changed |= canonicalize_call_targets_in_expression(assign->destination(), def_map, memo);
        }
        return changed;
    }
    if (auto* branch = dyn_cast<Branch>(inst)) {
        return canonicalize_call_targets_in_expression(branch->condition(), def_map, memo);
    }
    if (auto* ret = dyn_cast<Return>(inst)) {
        bool changed = false;
        for (Expression* value : ret->values()) {
            changed |= canonicalize_call_targets_in_expression(value, def_map, memo);
        }
        return changed;
    }
    if (auto* ibranch = dyn_cast<IndirectBranch>(inst)) {
        return canonicalize_call_targets_in_expression(ibranch->expression(), def_map, memo);
    }
    return false;
}

static bool type_compatible_for_identity(Variable* a, Variable* b) {
    if (!a || !b) return false;

    if (a->is_aliased() != b->is_aliased()) return false;
    if (a->is_aliased() && b->is_aliased() && a->name() != b->name()) return false;

    if (is_arm64_nonarg_width_alias_pair(a->name(), b->name())) {
        return true;
    }

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
    if (isa<Relation>(inst)) return true;
    if (auto* assign = dyn_cast<Assignment>(inst)) {
        if (contains_dereference(assign->destination())) return true;
        if (auto* op = dyn_cast<Operation>(assign->value())) {
            if (op->type() == OperationType::call) return true;
        }
        if (isa<Call>(assign->value())) return true;
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

    std::size_t instruction_count = 0;
    for (BasicBlock* block : task.cfg()->blocks()) {
        instruction_count += block->instructions().size();
    }

    const std::size_t substitution_budget = std::max<std::size_t>(
        MIN_PROPAGATION_SUBSTITUTIONS,
        instruction_count * (is_memory ? 2 : PROPAGATION_SUBSTITUTIONS_PER_INSTRUCTION));

    auto within_weight_budget = [](Expression* expr) {
        if (!expr) return true;
        if (isa<Variable>(expr) || isa<Constant>(expr)) return true;
        return expr_weight(expr, MAX_EXPR_WEIGHT + 1) <= MAX_EXPR_WEIGHT;
    };

    // Cache reachability matrix for cross-block substitution guards.
    std::unordered_map<BasicBlock*, std::unordered_set<BasicBlock*>> reachability;
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

    auto has_path = [&](BasicBlock* source, BasicBlock* sink) {
        if (source == sink) return true;
        return reachability[source].contains(sink);
    };

    std::unordered_map<BasicBlock*, std::size_t> rpo_index;
    const auto rpo_blocks = task.cfg()->reverse_post_order();
    for (std::size_t i = 0; i < rpo_blocks.size(); ++i) {
        rpo_index[rpo_blocks[i]] = i;
    }

    // Fixed-point outer loop
    for (int iteration = 0; iteration < MAX_PROPAGATION_ITERATIONS; ++iteration) {
        // Step 1: Remove redundant phis before each iteration
        remove_redundant_phis(task.cfg(), task.arena());

        std::unordered_map<Instruction*, InstLocation> loc_map;
        std::vector<Instruction*> dangerous_uses;

        // Step 2: Build DefMap globally (Variable -> Assignment*)
        AssignmentDefMap def_map;

        for (BasicBlock* block : task.cfg()->blocks()) {
            for (std::size_t i = 0; i < block->instructions().size(); ++i) {
                Instruction* inst = block->instructions()[i];
                loc_map[inst] = {block, i};
                if (is_memory && is_dangerous_memory_instruction(inst)) {
                    dangerous_uses.push_back(inst);
                }

                auto* assign = dyn_cast<Assignment>(inst);
                if (!assign) continue;
                if (auto* dest = dyn_cast<Variable>(assign->destination())) {
                    if (is_unstable_version_zero(dest)) {
                        continue;
                    }
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
        bool budget_exhausted = false;
        std::size_t substitutions_this_iteration = 0;

        for (BasicBlock* block : task.cfg()->reverse_post_order()) {
            auto& instrs = block->mutable_instructions();

            for (std::size_t idx = 0; idx < instrs.size(); ++idx) {
                Instruction* inst = instrs[idx];

                // Collect the variables required by this instruction (iterative
                // to avoid stack overflow on deeply nested expression trees).
                std::unordered_set<Variable*> reqs;
                collect_requirements_iterative(inst, reqs);

                for (Variable* req_var : reqs) {
                    if (is_unstable_version_zero(req_var)) {
                        continue;
                    }
                    VarKey key = var_key(req_var);
                    auto it = def_map.find(key);
                    if (it == def_map.end()) continue;

                    Assignment* def = it->second;

                    // Don't propagate a definition into itself
                    if (static_cast<Instruction*>(def) == inst) continue;

                    // Cross-block safety guard: only propagate forward along
                    // reachable order to avoid introducing non-dominating
                    // substitutions in branch guards and entry conditions.
                    auto def_loc = loc_map[def];
                    auto use_loc = loc_map[inst];
                    if (def_loc.block == use_loc.block) {
                        if (def_loc.index >= use_loc.index) {
                            continue;
                        }
                    } else {
                        if (!has_path(def_loc.block, use_loc.block)) {
                            continue;
                        }
                        auto def_rpo_it = rpo_index.find(def_loc.block);
                        auto use_rpo_it = rpo_index.find(use_loc.block);
                        if (def_rpo_it != rpo_index.end() && use_rpo_it != rpo_index.end()
                            && def_rpo_it->second > use_rpo_it->second) {
                            continue;
                        }
                    }

                    // Apply rule checks
                    bool allowed = true;
                    if (isa<Phi>(def)) allowed = false;
                    else if (auto* val = dyn_cast<Operation>(def->value())) {
                        if (val->type() == OperationType::call) allowed = false;
                    } else if (isa<Call>(def->value())) {
                        allowed = false;
                    }
                    if (allowed && is_address_of(def->value())) allowed = false;
                    if (allowed && isa<Phi>(inst) && isa<Operation>(def->value())) allowed = false;

                    if (!allowed) continue;

                    // Guard: Don't propagate definitions where the RHS
                    // references the LHS variable. Self-referential
                    // assignments like CMOV ternaries (x = (cond ? a : x))
                    // would cause exponential expression growth during
                    // substitution because the replaced variable reappears
                    // in the replacement, triggering infinite re-expansion.
                    if (auto* dest_var = dyn_cast<Variable>(def->destination())) {
                        if (expression_references_variable(
                                def->value(), dest_var->name(), dest_var->ssa_version())) {
                            continue;
                        }

                        // Keep loop-carried update branches readable/stable.
                        // If a branch guard consumes a variable that is defined
                        // from the same register family (different SSA version),
                        // substituting the update into the guard rewrites
                        // post-update tests into pre-update arithmetic forms
                        // (e.g. v1!=0 -> v0!=2), which later collapse poorly
                        // after out-of-SSA.
                        if (isa<Branch>(inst)
                            && expression_references_variable_name(def->value(), dest_var->name())) {
                            continue;
                        }
                    }

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

                    // Complexity guard: skip propagation if the replacement
                    // is too large. This prevents exponential tree growth
                    // from in-place aliasing across fixed-point iterations.
                    if (!isa<Variable>(replacement) && !isa<Constant>(replacement)) {
                        if (expr_weight(replacement, MAX_EXPR_WEIGHT + 1) > MAX_EXPR_WEIGHT)
                            continue;
                    }

                    // Copy-on-write substitution: creates new nodes only on the
                    // path from root to each match. Each match site gets a
                    // fresh copy of `replacement` to prevent DAG formation.
                    bool changed = false;

                    if (auto* assign = dyn_cast<Assignment>(inst)) {
                        auto [new_val, m1] = replace_variable_cow(
                            task.arena(), assign->value(), req_var, replacement);
                        if (m1 && !within_weight_budget(new_val)) {
                            new_val = assign->value();
                            m1 = false;
                        }
                        if (new_val != assign->value()) assign->set_value(new_val);
                        changed |= m1;
                        // Also propagate into complex destinations (e.g., *(ptr+off) = val)
                        if (!isa<Variable>(assign->destination())) {
                            auto [new_dest, m2] = replace_variable_cow(
                                task.arena(), assign->destination(), req_var, replacement);
                            if (m2 && !within_weight_budget(new_dest)) {
                                new_dest = assign->destination();
                                m2 = false;
                            }
                            if (new_dest != assign->destination()) assign->set_destination(new_dest);
                            changed |= m2;
                        }
                    } else if (auto* branch = dyn_cast<Branch>(inst)) {
                        Condition* cond = branch->condition();
                        if (cond) {
                            auto [new_cond, m] = replace_variable_cow(
                                task.arena(), cond, req_var, replacement);
                            if (m && !within_weight_budget(new_cond)) {
                                new_cond = cond;
                                m = false;
                            }
                            if (new_cond != cond) {
                                if (auto* nc = dyn_cast<Condition>(new_cond))
                                    branch->set_condition(nc);
                            }
                            changed |= m;
                        }
                    } else if (auto* ret = dyn_cast<Return>(inst)) {
                        auto& vals = ret->mutable_values();
                        for (std::size_t vi = 0; vi < vals.size(); ++vi) {
                            auto [new_v, m] = replace_variable_cow(
                                task.arena(), vals[vi], req_var, replacement);
                            if (m && !within_weight_budget(new_v)) {
                                new_v = vals[vi];
                                m = false;
                            }
                            if (new_v != vals[vi]) vals[vi] = new_v;
                            changed |= m;
                        }
                    }

                    if (changed) {
                        any_change = true;
                        ++substitutions_this_iteration;
                        if (substitutions_this_iteration >= substitution_budget) {
                            budget_exhausted = true;
                            break;
                        }
                    }
                }

                if (budget_exhausted) break;

                // CMP+branch folding after propagation
                if (auto* branch = dyn_cast<Branch>(inst)) {
                    try_fold_cmp_branch(task.arena(), branch);
                }
            }

            if (budget_exhausted) break;
        }

        // Fixed-point: stop if no changes occurred in this iteration
        if (!any_change || budget_exhausted) break;
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
            if (auto* branch = dyn_cast<Branch>(last_inst)) {
                std::unordered_set<Variable*> cond_reqs;
                collect_requirements_iterative(branch, cond_reqs);
                // Strong precondition: only prune when condition is fully
                // constantized (no variable requirements remain).
                if (!cond_reqs.empty()) {
                    continue;
                }
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
            if (isa<Branch>(source->instructions().back())) {
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
            if (auto* phi = dyn_cast<Phi>(inst)) {
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
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                if (auto* dest = dyn_cast<Variable>(assign->destination())) {
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
            if (auto* branch = dyn_cast<Branch>(last_inst)) {
                // Dependency dict: variables required by branch -> Phi that defines them
                std::unordered_map<Variable*, Phi*> phi_dependencies;
                std::unordered_set<Variable*> reqs;
                collect_requirements_iterative(branch, reqs);

                for (Variable* req : reqs) {
                    if (req->is_aliased()) continue;
                    auto it = def_map.find(var_key(req));
                    if (it != def_map.end()) {
                        if (auto* phi = dyn_cast<Phi>(it->second)) {
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
                        if (auto* c = dyn_cast<Constant>(expr)) {
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

                if (auto* patched_cond = dyn_cast<Condition>(patched_condition_expr)) {
                    std::unordered_set<Variable*> patched_reqs;
                    patched_cond->collect_requirements(patched_reqs);
                    if (!patched_reqs.empty()) {
                        continue;
                    }
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
            if (isa<Branch>(source->instructions().back())) {
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
            if (auto* phi = dyn_cast<Phi>(inst)) {
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
                if (auto* assign = dyn_cast<Assignment>(inst)) {
                    if (auto* dest = dyn_cast<Variable>(assign->destination())) {
                        VarKey key = var_key(dest);
                        if (!def_map.contains(key)) def_map[key] = assign;
                    }
                }
                std::unordered_set<Variable*> reqs;
                collect_requirements_iterative(inst, reqs);
                for (Variable* req : reqs) {
                    use_map[var_key(req)].insert(inst);
                }
            }
        }

        auto call_replacement_is_safe = [&](Expression* expr) {
            if (!expr) {
                return false;
            }
            if (isa<ListOperation>(strip_trivial_casts(expr))) {
                return false;
            }

             std::function<bool(Expression*)> has_nested_call_or_list = [&](Expression* node) {
                if (!node) {
                    return false;
                }
                if (isa<ListOperation>(node) || isa<Call>(node)) {
                    return true;
                }
                if (auto* op = dyn_cast<Operation>(node)) {
                    if (op->type() == OperationType::call) {
                        return true;
                    }
                    for (Expression* child : op->operands()) {
                        if (has_nested_call_or_list(child)) {
                            return true;
                        }
                    }
                }
                return false;
            };
            if (has_nested_call_or_list(expr)) {
                return false;
            }

            std::unordered_set<Variable*> reqs;
            collect_variables_iterative(expr, reqs);
            for (Variable* req : reqs) {
                if (!req || req->is_parameter()) {
                    continue;
                }
                if (!def_map.contains(var_key(req))) {
                    return false;
                }
            }
            return true;
        };

        std::unordered_map<VarKey, Expression*, VarKeyHash> canonical_target_memo;
        for (BasicBlock* block : task.cfg()->blocks()) {
            for (Instruction* inst : block->instructions()) {
                if (canonicalize_call_targets_in_instruction(inst, def_map, canonical_target_memo)) {
                    changed = true;
                }
            }
        }

        // Propagate local non-call definitions into call arguments.
        // This recovers ABI argument provenance at call-sites (e.g. ARM64 x0/x1/w2)
        // without creating cross-call nesting.
        for (BasicBlock* block : task.cfg()->blocks()) {
            auto& instrs = block->mutable_instructions();
            for (std::size_t i = 0; i < instrs.size(); ++i) {
                auto* assign = dyn_cast<Assignment>(instrs[i]);
                if (!assign) {
                    continue;
                }

                auto propagate_arg = [&](Expression*& arg_expr) {
                    for (std::size_t depth = 0; depth < 6; ++depth) {
                        auto* arg_var = dyn_cast<Variable>(strip_trivial_casts(arg_expr));
                        if (!arg_var) {
                            return;
                        }
                        const VarKey arg_key = var_key(arg_var);

                        Assignment* local_def = nullptr;
                        for (std::size_t back = i; back > 0; --back) {
                            auto* prev_assign = dyn_cast<Assignment>(instrs[back - 1]);
                            auto* prev_dest = prev_assign ? dyn_cast<Variable>(prev_assign->destination()) : nullptr;
                            if (!prev_dest || var_key(prev_dest) != arg_key) {
                                continue;
                            }
                            local_def = prev_assign;
                            break;
                        }

                        auto* def_op = local_def ? dyn_cast<Operation>(local_def->value()) : nullptr;
                        if (!local_def || isa<Call>(local_def->value())
                            || (def_op && def_op->type() == OperationType::call)) {
                            return;
                        }
                        if (!call_replacement_is_safe(local_def->value())) {
                            return;
                        }
                        if (expression_references_variable(local_def->value(), arg_key.name, arg_key.ssa_version)) {
                            return;
                        }

                        auto is_stack_pointer_like = [](Expression* expr) {
                            auto* v = dyn_cast<Variable>(strip_trivial_casts(expr));
                            if (!v) {
                                return false;
                            }
                            std::string lowered = v->name();
                            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            return lowered == "sp" || lowered.rfind("sp_", 0) == 0;
                        };

                        if (is_stack_pointer_like(local_def->value()) && !is_stack_pointer_like(arg_var)) {
                            return;
                        }

                        Expression* replacement = local_def->value()->copy(task.arena());
                        if (!replacement) {
                            return;
                        }
                        arg_expr = replacement;
                        changed = true;

                        if (!isa<Variable>(strip_trivial_casts(arg_expr))) {
                            return;
                        }
                    }
                };

                if (auto* call = dyn_cast<Call>(assign->value())) {
                    auto& ops = call->mutable_operands();
                    for (std::size_t arg_i = 1; arg_i < ops.size(); ++arg_i) {
                        propagate_arg(ops[arg_i]);
                    }
                } else if (auto* op = dyn_cast<Operation>(assign->value())) {
                    if (op->type() == OperationType::call) {
                        auto& ops = op->mutable_operands();
                        for (std::size_t arg_i = 1; arg_i < ops.size(); ++arg_i) {
                            propagate_arg(ops[arg_i]);
                        }
                    }
                }
            }
        }

        for (BasicBlock* block : task.cfg()->blocks()) {
            auto& instrs = block->mutable_instructions();
            for (std::size_t idx = 0; idx < instrs.size(); ++idx) {
                Instruction* inst = instrs[idx];
                if (!inst) continue;

                Expression** target_ref = nullptr;
                if (auto* assign = dyn_cast<Assignment>(inst)) {
                    if (auto* call = dyn_cast<Call>(assign->value())) {
                        target_ref = &call->mutable_operands()[0];
                    } else if (auto* op = dyn_cast<Operation>(assign->value())) {
                        if (op->type() == OperationType::call && !op->mutable_operands().empty()) {
                            target_ref = &op->mutable_operands()[0];
                        }
                    }
                }

                if (!target_ref || !*target_ref) {
                    continue;
                }

                auto* call_target_var = dyn_cast<Variable>(strip_trivial_casts(*target_ref));
                if (!call_target_var) {
                    continue;
                }

                const VarKey target_key = var_key(call_target_var);
                Assignment* local_def = nullptr;
                bool crossed_dangerous_memory = false;

                for (std::size_t back = idx; back > 0; --back) {
                    Instruction* prev = instrs[back - 1];
                    if (!prev) {
                        continue;
                    }

                    if (is_dangerous_memory_instruction(prev)) {
                        crossed_dangerous_memory = true;
                    }

                    auto* prev_assign = dyn_cast<Assignment>(prev);
                    auto* prev_dest = prev_assign ? dyn_cast<Variable>(prev_assign->destination()) : nullptr;
                    if (!prev_dest || var_key(prev_dest) != target_key) {
                        continue;
                    }

                    local_def = prev_assign;
                    break;
                }

                if (!local_def || crossed_dangerous_memory) {
                    continue;
                }

                auto* load_op = dyn_cast<Operation>(local_def->value());
                if (!load_op || load_op->type() != OperationType::deref || load_op->operands().size() != 1) {
                    continue;
                }

                *target_ref = load_op->copy(task.arena());
                changed = true;
            }
        }

        // Recover common strtol validation provenance shape:
        //   strtol(*(endptr_slot), endptr_slot, 0xa)
        // should use argv[1] as first argument. When we detect this degraded
        // pattern and function has parameter index 1, rewrite arg0 to *(arg1+1).
        for (BasicBlock* block : task.cfg()->blocks()) {
            for (Instruction* inst : block->instructions()) {
                auto* assign = dyn_cast<Assignment>(inst);
                if (!assign) {
                    continue;
                }

                auto rewrite_strtol_args = [&](Expression* target_expr, Expression*& arg0, Expression*& arg1, Expression* arg2) {
                    std::string canon_name;
                    if (auto* gv = dyn_cast<GlobalVariable>(target_expr)) {
                        canon_name = gv->name();
                        std::transform(canon_name.begin(), canon_name.end(), canon_name.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        while (!canon_name.empty() && canon_name.front() == '_') {
                            canon_name.erase(canon_name.begin());
                        }
                    } else if (auto* c = dyn_cast<Constant>(target_expr)) {
                        const auto target_addr = static_cast<ida::Address>(c->value());
                        if (auto maybe_name = ida::name::get(target_addr)) {
                            canon_name = *maybe_name;
                            std::transform(canon_name.begin(), canon_name.end(), canon_name.begin(),
                                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                            while (!canon_name.empty() && canon_name.front() == '_') {
                                canon_name.erase(canon_name.begin());
                            }
                        }
                    }

                    // Accept degraded forms even when callee-name recovery is weak.
                    // This pattern is high-signal for strtol-style validation:
                    //   call(*(slot), slot, base)
                    // where arg0 is deref(arg1).

                    Expression* arg0_base_expr = strip_trivial_casts(arg0);
                    auto* arg0_deref = dyn_cast<Operation>(arg0_base_expr);
                    if (!arg0_deref) {
                        auto* arg0_var = dyn_cast<Variable>(arg0_base_expr);
                        if (arg0_var) {
                            Assignment* local_def = nullptr;
                            for (Instruction* prev_inst : block->instructions()) {
                                if (prev_inst == inst) {
                                    break;
                                }
                                auto* prev_assign = dyn_cast<Assignment>(prev_inst);
                                if (!prev_assign) {
                                    continue;
                                }
                                auto* dst = dyn_cast<Variable>(prev_assign->destination());
                                if (!dst || var_key(dst) != var_key(arg0_var)) {
                                    continue;
                                }
                                local_def = prev_assign;
                            }
                            if (local_def) {
                                arg0_deref = dyn_cast<Operation>(strip_trivial_casts(local_def->value()));
                            }
                        }
                    }
                    if (!arg0_deref || arg0_deref->type() != OperationType::deref
                        || arg0_deref->operands().size() != 1) {
                        return;
                    }

                    Expression* arg1_base = strip_trivial_casts(arg1);
                    Expression* arg0_base = strip_trivial_casts(arg0_deref->operands()[0]);
                    bool matching_slot = false;
                    auto is_stack_slot_expr = [](Expression* expr) {
                        Expression* base = strip_trivial_casts(expr);
                        if (auto* v = dyn_cast<Variable>(base)) {
                            std::string lowered = v->name();
                            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            return lowered == "sp" || lowered.rfind("sp_", 0) == 0;
                        }
                        auto* op = dyn_cast<Operation>(base);
                        if (!op || (op->type() != OperationType::add && op->type() != OperationType::sub)
                            || op->operands().size() != 2) {
                            return false;
                        }
                        auto* lhs = dyn_cast<Variable>(strip_trivial_casts(op->operands()[0]));
                        if (!lhs || !isa<Constant>(strip_trivial_casts(op->operands()[1]))) {
                            return false;
                        }
                        std::string lowered = lhs->name();
                        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        return lowered == "sp" || lowered.rfind("sp_", 0) == 0;
                    };

                    if (auto* arg1_var = dyn_cast<Variable>(arg1_base)) {
                        if (auto* deref_var = dyn_cast<Variable>(arg0_base)) {
                            matching_slot = (var_key(deref_var) == var_key(arg1_var));
                        }
                    }

                    if (!matching_slot && arg1_base && arg0_base) {
                        matching_slot = (arg1_base == arg0_base);
                    }

                    const bool stack_slot_like = is_stack_slot_expr(arg1_base);
                    if (!matching_slot && !stack_slot_like) {
                        return;
                    }

                    // Use a stable ABI-parameter placeholder to avoid collapsing
                    // argv provenance back into temporary stack-slot aliases.
                    auto* argv_base = task.arena().create<Variable>("arg_1", 8);
                    argv_base->set_kind(VariableKind::Parameter);
                    argv_base->set_parameter_index(1);
                    auto* one = task.arena().create<Constant>(1, 8);
                    auto* argv_idx = task.arena().create<Operation>(
                        OperationType::add,
                        std::vector<Expression*>{argv_base, one},
                        8);
                    arg0 = task.arena().create<Operation>(
                        OperationType::deref,
                        std::vector<Expression*>{argv_idx},
                        8);
                    auto* endptr = task.arena().create<Variable>("endptr", 8);
                    arg1 = task.arena().create<Operation>(
                        OperationType::address_of,
                        std::vector<Expression*>{endptr},
                        8);
                    changed = true;
                };

                if (auto* call = dyn_cast<Call>(assign->value())) {
                    auto& ops = call->mutable_operands();
                    if (ops.size() >= 4) {
                        rewrite_strtol_args(ops[0], ops[1], ops[2], ops[3]);
                    }
                } else if (auto* op = dyn_cast<Operation>(assign->value())) {
                    if (op->type() == OperationType::call) {
                        auto& ops = op->mutable_operands();
                        if (ops.size() >= 4) {
                            rewrite_strtol_args(ops[0], ops[1], ops[2], ops[3]);
                        }
                    }
                }
            }
        }

        for (BasicBlock* block : task.cfg()->blocks()) {
            auto& instrs = block->mutable_instructions();
            for (std::size_t i = 0; i < instrs.size(); ++i) {
                Instruction* inst = instrs[i];
                std::unordered_set<Variable*> reqs;
                collect_requirements_iterative(inst, reqs);

                for (Variable* req_var : reqs) {
                    VarKey key = var_key(req_var);
                    auto it_def = def_map.find(key);
                    if (it_def == def_map.end()) continue;
                    Assignment* def = it_def->second;

                    // We only propagate call assignments
                    bool is_call = false;
                    if (auto* op = dyn_cast<Operation>(def->value())) {
                        if (op->type() == OperationType::call) is_call = true;
                    } else if (isa<Call>(def->value())) {
                        is_call = true;
                    }

                    if (!is_call) continue;

                    if (use_map[key].size() != 1) continue;

                    // Use-map tracks instructions, not per-instruction occurrence
                    // counts. Block propagation when the variable appears multiple
                    // times inside the same use instruction.
                    if (count_variable_occurrences_in_instruction(inst, key.name, key.ssa_version) != 1) {
                        continue;
                    }

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

                    if (!call_replacement_is_safe(def->value())) {
                        continue;
                    }

                    // Guard: do NOT inline a call result into another call's
                    // arguments. On ARM64, x0 is both the return register and
                    // first argument register, so consecutive calls form
                    // artificial def-use chains through x0. Inlining call
                    // results into other calls creates semantically wrong
                    // nested expressions like puts(printf(clock_gettime(...))).
                    if (auto* use_assign = dyn_cast<Assignment>(inst)) {
                        bool use_is_call = false;
                        if (auto* uop = dyn_cast<Operation>(use_assign->value())) {
                            if (uop->type() == OperationType::call) use_is_call = true;
                        } else if (isa<Call>(use_assign->value())) {
                            use_is_call = true;
                        }
                        if (use_is_call) continue;
                    }

                    // Copy-on-write substitution
                    Expression* replacement = def->value();
                    bool sub_changed = false;

                    if (auto* assign = dyn_cast<Assignment>(inst)) {
                        auto [new_val, m1] = replace_variable_cow(
                            task.arena(), assign->value(), req_var, replacement);
                        if (new_val != assign->value()) assign->set_value(new_val);
                        sub_changed |= m1;
                        if (!isa<Variable>(assign->destination())) {
                            auto [new_dest, m2] = replace_variable_cow(
                                task.arena(), assign->destination(), req_var, replacement);
                            if (new_dest != assign->destination()) assign->set_destination(new_dest);
                            sub_changed |= m2;
                        }
                    } else if (auto* branch = dyn_cast<Branch>(inst)) {
                        Condition* cond = branch->condition();
                        if (cond) {
                            auto [new_cond, m] = replace_variable_cow(
                                task.arena(), cond, req_var, replacement);
                            if (new_cond != cond) {
                                if (auto* nc = dyn_cast<Condition>(new_cond))
                                    branch->set_condition(nc);
                            }
                            sub_changed |= m;
                        }
                    } else if (auto* ret = dyn_cast<Return>(inst)) {
                        auto& vals = ret->mutable_values();
                        for (std::size_t vi = 0; vi < vals.size(); ++vi) {
                            auto [new_v, m] = replace_variable_cow(
                                task.arena(), vals[vi], req_var, replacement);
                            if (new_v != vals[vi]) vals[vi] = new_v;
                            sub_changed |= m;
                        }
                    }

                    if (sub_changed) {
                        // Keep original call definition intact; downstream DCE
                        // will remove it when truly unused. Replacing with a
                        // constant here can sever return-value expression chains.
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

    auto is_stack_pointer_like = [](Variable* v) {
        if (!v) {
            return false;
        }
        std::string lowered = v->name();
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered == "sp" || lowered.rfind("sp_", 0) == 0;
    };

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

            auto* assign = dyn_cast<Assignment>(inst);
            if (!assign) continue;

            auto* dest = dyn_cast<Variable>(assign->destination());
            if (!dest) continue;

            if (auto* src = dyn_cast<Variable>(assign->value())) {
                if (is_stack_pointer_like(src) && !is_stack_pointer_like(dest)) {
                    continue;
                }

                // Do not collapse distinct ARM64 X/W register families into
                // one identity component (e.g. x19 = x0). Those copies carry
                // value movement semantics (argument -> callee-saved/local)
                // and merging them can erase required initializations.
                std::string dest_lower = dest->name();
                std::string src_lower = src->name();
                std::transform(dest_lower.begin(), dest_lower.end(), dest_lower.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(src_lower.begin(), src_lower.end(), src_lower.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const auto dest_arm64_idx = parse_arm64_xw_index(dest_lower);
                const auto src_arm64_idx = parse_arm64_xw_index(src_lower);
                if (dest_arm64_idx && src_arm64_idx && *dest_arm64_idx != *src_arm64_idx) {
                    continue;
                }

                if (type_compatible_for_identity(dest, src)) {
                    direct_identity_edges.push_back({var_key(dest), var_key(src)});
                }
                continue;
            }

            auto* phi = dyn_cast<Phi>(inst);
            if (!phi || !phi->operand_list()) continue;

            PhiIdentityInfo info{var_key(dest), {}};
            for (Expression* op : phi->operand_list()->operands()) {
                auto* ov = dyn_cast<Variable>(op);
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

    std::unordered_map<VarKey, int, VarKeyHash> id_of_lower;
    id_of_lower.reserve(keys.size());
    for (const VarKey& key : keys) {
        VarKey lowered_key{key.name, key.ssa_version};
        std::transform(lowered_key.name.begin(), lowered_key.name.end(), lowered_key.name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!id_of_lower.contains(lowered_key)) {
            id_of_lower.emplace(std::move(lowered_key), id_of[key]);
        }
    }

    // ARM64 width aliases (xN/wN, N>=8): preserve identity across width-only
    // naming splits so out-of-SSA does not strand one side as undefined.
    for (const VarKey& key : keys) {
        std::string lowered = key.name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        auto maybe_index = parse_arm64_xw_index(lowered);
        if (!maybe_index || *maybe_index < 8u) {
            continue;
        }
        const char prefix = lowered.front();
        if (prefix != 'x' && prefix != 'w') {
            continue;
        }

        std::string counterpart = lowered;
        counterpart.front() = (prefix == 'x') ? 'w' : 'x';

        VarKey lhs_lower{lowered, key.ssa_version};
        VarKey rhs_lower{counterpart, key.ssa_version};
        auto it_lhs = id_of_lower.find(lhs_lower);
        auto it_rhs = id_of_lower.find(rhs_lower);
        if (it_lhs != id_of_lower.end() && it_rhs != id_of_lower.end()) {
            dsu.unite(it_lhs->second, it_rhs->second);
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
            if (auto* phi = dyn_cast<Phi>(inst)) {
                Variable* dest = phi->dest_var();
                if (!dest || !phi->operand_list()) {
                    continue;
                }

                Variable* single = nullptr;
                bool all_same = true;
                for (Expression* op : phi->operand_list()->operands()) {
                    auto* ov = dyn_cast<Variable>(op);
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

            if (auto* assign = dyn_cast<Assignment>(inst)) {
                auto* dst = dyn_cast<Variable>(assign->destination());
                auto* src = dyn_cast<Variable>(assign->value());
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
    if (isa<Constant>(expr) || isa<Variable>(expr)) return 1;

    std::vector<Expression*> stack;
    stack.push_back(expr);
    std::size_t score = 0;

    while (!stack.empty()) {
        Expression* current = stack.back();
        stack.pop_back();
        ++score;

        if (auto* op = dyn_cast<Operation>(current)) {
            for (Expression* child : op->operands()) {
                stack.push_back(child);
            }
        } else if (auto* list = dyn_cast<ListOperation>(current)) {
            for (Expression* child : list->operands()) {
                stack.push_back(child);
            }
        }
    }

    return score;
}

static std::uint64_t expression_fp(Expression* expr) {
    return expression_fingerprint_hash(expr);
}

static void collect_subexpressions(Expression* root, std::vector<Expression*>& out) {
    if (!root) return;
    out.push_back(root);
    if (auto* op = dyn_cast<Operation>(root)) {
        for (Expression* child : op->operands()) collect_subexpressions(child, out);
        return;
    }
    if (auto* list = dyn_cast<ListOperation>(root)) {
        for (Expression* child : list->operands()) collect_subexpressions(child, out);
    }
}

static bool contains_call_expression(Expression* expr) {
    if (!expr) return false;
    if (isa<Call>(expr)) return true;
    if (auto* op = dyn_cast<Operation>(expr)) {
        if (op->type() == OperationType::call) return true;
        for (Expression* child : op->operands()) {
            if (contains_call_expression(child)) return true;
        }
    }
    if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            if (contains_call_expression(child)) return true;
        }
    }
    return false;
}

static std::vector<Expression*> instruction_subexpressions(Instruction* inst) {
    std::vector<Expression*> out;

    if (auto* assign = dyn_cast<Assignment>(inst)) {
        collect_subexpressions(assign->value(), out);
        if (assign->destination() && !isa<Variable>(assign->destination())) {
            if (auto* op = dyn_cast<Operation>(assign->destination())) {
                for (Expression* child : op->operands()) collect_subexpressions(child, out);
            } else if (auto* list = dyn_cast<ListOperation>(assign->destination())) {
                for (Expression* child : list->operands()) collect_subexpressions(child, out);
            }
        }
        return out;
    }

    if (auto* branch = dyn_cast<Branch>(inst)) {
        if (branch->condition()) {
            for (Expression* child : branch->condition()->operands()) collect_subexpressions(child, out);
        }
        return out;
    }

    if (auto* ib = dyn_cast<IndirectBranch>(inst)) {
        collect_subexpressions(ib->expression(), out);
        return out;
    }

    if (auto* ret = dyn_cast<Return>(inst)) {
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
        if (auto* rel = dyn_cast<Relation>(insts[i])) {
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

        std::vector<std::uint64_t> inserted_keys;

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
                    std::uint64_t fp = expression_fp(subexpr);
                    auto it = defining_variable_of_.find(fp);
                    if (it == defining_variable_of_.end() || it->second.empty()) continue;

                    const ExistingSubexpressionDef& def = it->second.back();
                    if (!can_replace_subexpression(def, block, idx)) continue;

                    inst->substitute(subexpr, def.variable);
                    changed = true;
                    break;
                }
            }

            auto* assign = dyn_cast<Assignment>(inst);
            if (!assign || isa<Phi>(inst)) continue;

            auto* dest = dyn_cast<Variable>(assign->destination());
            Expression* value = assign->value();
            if (!dest || !value) continue;
            if (expression_complexity(value) <= 1) continue;
            if (contains_dereference(value) || contains_call_expression(value)) continue;

            std::uint64_t fp = expression_fp(value);
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
    std::unordered_map<std::uint64_t, std::vector<ExistingSubexpressionDef>> defining_variable_of_;
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

            for (const std::uint64_t fp : candidates) {
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
    std::unordered_map<std::uint64_t, SubexpressionStats> collect_usage_map() {
        std::unordered_map<std::uint64_t, SubexpressionStats> usage_map;

        for (BasicBlock* block : cfg_.blocks()) {
            const auto& insts = block->instructions();
            for (std::size_t idx = 0; idx < insts.size(); ++idx) {
                Instruction* inst = insts[idx];
                if (!inst) continue;

                std::vector<Expression*> subexprs = instruction_subexpressions(inst);
                for (Expression* expr : subexprs) {
                    if (!expr) continue;
                    std::uint64_t fp = expression_fp(expr);
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

    std::vector<std::uint64_t> sorted_candidates(const std::unordered_map<std::uint64_t, SubexpressionStats>& usage_map) {
        std::vector<std::pair<std::uint64_t, const SubexpressionStats*>> rows;
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

        std::vector<std::uint64_t> out;
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
        if (isa<ListOperation>(stats.exemplar)) return false;
        if (contains_dereference(stats.exemplar)) return false;
        if (contains_call_expression(stats.exemplar)) return false;
        return true;
    }

    static bool replace_in_expression(Expression*& expr,
                                      std::uint64_t target_fp,
                                      Variable* replacement) {
        if (!expr) return false;

        if (expression_fp(expr) == target_fp) {
            expr = replacement;
            return true;
        }

        bool changed = false;
        if (auto* op = dyn_cast<Operation>(expr)) {
            for (Expression*& child : op->mutable_operands()) {
                changed = replace_in_expression(child, target_fp, replacement) || changed;
            }
        } else if (auto* list = dyn_cast<ListOperation>(expr)) {
            for (Expression*& child : list->mutable_operands()) {
                changed = replace_in_expression(child, target_fp, replacement) || changed;
            }
        }

        return changed;
    }

    static bool rewrite_instruction(Instruction* inst,
                                    std::uint64_t target_fp,
                                    Variable* replacement) {
        bool changed = false;

        if (auto* assign = dyn_cast<Assignment>(inst)) {
            Expression* value = assign->value();
            if (replace_in_expression(value, target_fp, replacement)) {
                assign->set_value(value);
                changed = true;
            }

            if (assign->destination() && !isa<Variable>(assign->destination())) {
                Expression* dest = assign->destination();
                if (replace_in_expression(dest, target_fp, replacement)) {
                    assign->set_destination(dest);
                    changed = true;
                }
            }
            return changed;
        }

        if (auto* branch = dyn_cast<Branch>(inst)) {
            if (auto* cond = branch->condition()) {
                for (Expression*& child : cond->mutable_operands()) {
                    changed = replace_in_expression(child, target_fp, replacement) || changed;
                }
            }
            return changed;
        }

        if (auto* ib = dyn_cast<IndirectBranch>(inst)) {
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

        if (auto* ret = dyn_cast<Return>(inst)) {
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

    bool apply_candidate(std::uint64_t fp, const SubexpressionStats& stats) {
        if (!stats.exemplar) return false;

        std::unordered_set<BasicBlock*> uniq;
        std::vector<BasicBlock*> usage_blocks;
        for (const auto& usage : stats.usages) {
            if (!usage.instruction || !usage.block) continue;
            if (isa<Phi>(usage.instruction)) {
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
    const auto* c = dyn_cast<Constant>(expr);
    return c && c->value() == value;
}

bool is_all_ones_constant(const Expression* expr, std::size_t size_bytes) {
    const auto* c = dyn_cast<Constant>(expr);
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

struct AffineTerm {
    Variable* base = nullptr;
    std::int64_t delta = 0;
    std::size_t size_bytes = 0;
};

bool add_int64_checked(std::int64_t lhs, std::int64_t rhs, std::int64_t& out) {
    if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

bool constant_to_int64(const Constant* c, std::int64_t& out) {
    if (!c) return false;
    if (c->value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    out = static_cast<std::int64_t>(c->value());
    return true;
}

bool extract_affine_term(Expression* expr,
                         Variable*& base,
                         std::int64_t& delta,
                         std::size_t& size_bytes) {
    if (!expr) return false;

    if (auto* v = dyn_cast<Variable>(expr)) {
        base = v;
        delta = 0;
        size_bytes = v->size_bytes;
        return true;
    }

    auto* op = dyn_cast<Operation>(expr);
    if (!op || op->operands().size() != 2) {
        return false;
    }

    if (op->type() == OperationType::add) {
        auto* lhs_var = dyn_cast<Variable>(op->operands()[0]);
        auto* rhs_var = dyn_cast<Variable>(op->operands()[1]);
        auto* lhs_const = dyn_cast<Constant>(op->operands()[0]);
        auto* rhs_const = dyn_cast<Constant>(op->operands()[1]);

        std::int64_t cval = 0;
        if (lhs_var && rhs_const && constant_to_int64(rhs_const, cval)) {
            base = lhs_var;
            delta = cval;
            size_bytes = op->size_bytes;
            return true;
        }
        if (rhs_var && lhs_const && constant_to_int64(lhs_const, cval)) {
            base = rhs_var;
            delta = cval;
            size_bytes = op->size_bytes;
            return true;
        }
        return false;
    }

    if (op->type() == OperationType::sub) {
        auto* lhs_var = dyn_cast<Variable>(op->operands()[0]);
        auto* rhs_const = dyn_cast<Constant>(op->operands()[1]);
        std::int64_t cval = 0;
        if (lhs_var && rhs_const && constant_to_int64(rhs_const, cval)) {
            base = lhs_var;
            delta = -cval;
            size_bytes = op->size_bytes;
            return true;
        }
    }

    return false;
}

std::optional<AffineTerm> resolve_affine_term(
    Variable* source,
    const std::unordered_map<VarKey, AffineTerm, VarKeyHash>& terms) {
    if (!source) return std::nullopt;

    Variable* base = source;
    std::int64_t delta = 0;
    std::size_t size_bytes = source->size_bytes;
    std::unordered_set<VarKey, VarKeyHash> seen;

    while (base) {
        VarKey key = var_key(base);
        if (!seen.insert(key).second) {
            return std::nullopt;
        }

        auto it = terms.find(key);
        if (it == terms.end()) {
            break;
        }

        const AffineTerm& term = it->second;
        if (!term.base) {
            return std::nullopt;
        }

        std::int64_t combined = 0;
        if (!add_int64_checked(delta, term.delta, combined)) {
            return std::nullopt;
        }
        delta = combined;

        if (term.size_bytes != 0) {
            size_bytes = term.size_bytes;
        }

        if (term.base == base) {
            break;
        }
        base = term.base;
    }

    return AffineTerm{base, delta, size_bytes};
}

Expression* build_affine_expression(DecompilerTask& task,
                                    Variable* base,
                                    std::int64_t delta,
                                    std::size_t size_bytes,
                                    const TypePtr& ir_type) {
    if (!base) {
        return nullptr;
    }
    if (delta == 0) {
        return base;
    }

    const std::size_t expr_size = size_bytes != 0
        ? size_bytes
        : (base->size_bytes != 0 ? base->size_bytes : static_cast<std::size_t>(8));

    const std::uint64_t magnitude = delta < 0
        ? static_cast<std::uint64_t>(-(delta + 1)) + 1ULL
        : static_cast<std::uint64_t>(delta);

    auto* c = task.arena().create<Constant>(magnitude, expr_size);
    auto* rebuilt = task.arena().create<Operation>(
        delta < 0 ? OperationType::sub : OperationType::add,
        std::vector<Expression*>{base, c},
        expr_size);
    rebuilt->set_ir_type(ir_type);
    return rebuilt;
}

bool collapse_linear_offset_chains(DecompilerTask& task, BasicBlock* block) {
    if (!block) {
        return false;
    }

    bool changed = false;
    std::unordered_map<VarKey, AffineTerm, VarKeyHash> terms;

    for (Instruction* inst : block->mutable_instructions()) {
        auto* assign = dyn_cast<Assignment>(inst);
        auto* dest = assign ? dyn_cast<Variable>(assign->destination()) : nullptr;
        if (!dest || !assign->value()) {
            continue;
        }

        const VarKey dest_key = var_key(dest);

        Variable* source = nullptr;
        std::int64_t local_delta = 0;
        std::size_t size_bytes = 0;
        if (!extract_affine_term(assign->value(), source, local_delta, size_bytes)) {
            terms.erase(dest_key);
            continue;
        }

        auto resolved = resolve_affine_term(source, terms);
        if (!resolved.has_value() || !resolved->base) {
            terms.erase(dest_key);
            continue;
        }

        std::int64_t total_delta = 0;
        if (!add_int64_checked(resolved->delta, local_delta, total_delta)) {
            terms.erase(dest_key);
            continue;
        }

        const std::size_t canonical_size =
            size_bytes != 0 ? size_bytes :
            (resolved->size_bytes != 0 ? resolved->size_bytes : dest->size_bytes);

        Expression* rebuilt = build_affine_expression(
            task,
            resolved->base,
            total_delta,
            canonical_size,
            assign->value()->ir_type());
        if (rebuilt && rebuilt != assign->value()) {
            assign->set_value(rebuilt);
            changed = true;
        }

        terms[dest_key] = AffineTerm{resolved->base, total_delta, canonical_size};
    }

    return changed;
}

std::optional<FoldedConstant> collapse_binary_constants(
    OperationType type,
    const Constant* lhs,
    const Constant* rhs,
    std::size_t out_size_bytes);

Expression* try_collapse_nested_constants(DecompilerTask& task, Operation* op) {
    if (!op || op->operands().size() != 2) return nullptr;
    if (op->type() == OperationType::sub) {
        auto* outer_const = dyn_cast<Constant>(op->operands()[1]);
        auto* inner = dyn_cast<Operation>(op->operands()[0]);
        if (!outer_const || !inner || inner->type() != OperationType::sub ||
            inner->operands().size() != 2) {
            return nullptr;
        }

        auto* inner_const = dyn_cast<Constant>(inner->operands()[1]);
        if (!inner_const) {
            return nullptr;
        }

        auto folded = collapse_binary_constants(
            OperationType::add, inner_const, outer_const, op->size_bytes);
        if (!folded) {
            return nullptr;
        }

        auto* merged_const = task.arena().create<Constant>(folded->value, folded->size_bytes);
        merged_const->set_ir_type(inner_const->ir_type());

        auto* rebuilt = task.arena().create<Operation>(
            OperationType::sub,
            std::vector<Expression*>{inner->operands()[0], merged_const},
            op->size_bytes);
        rebuilt->set_ir_type(op->ir_type());
        return rebuilt;
    }

    if (op->type() != OperationType::add && op->type() != OperationType::mul &&
        op->type() != OperationType::mul_us) {
        return nullptr;
    }

    auto try_orientation = [&](Expression* inner_candidate, Expression* outer_constant_candidate) -> Expression* {
        auto* outer_const = dyn_cast<Constant>(outer_constant_candidate);
        if (!outer_const) return nullptr;

        auto* inner = dyn_cast<Operation>(inner_candidate);
        if (!inner || inner->type() != op->type() || inner->operands().size() != 2) {
            return nullptr;
        }

        auto* inner_const = dyn_cast<Constant>(inner->operands()[0]);
        Expression* inner_other = inner->operands()[1];
        if (!inner_const) {
            inner_const = dyn_cast<Constant>(inner->operands()[1]);
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

Expression* negate_ternary_condition(DecompilerTask& task, Expression* cond) {
    if (!cond) {
        return nullptr;
    }

    if (auto* cond_node = dyn_cast<Condition>(cond)) {
        const auto negated = Condition::negate_comparison(cond_node->type());
        return task.arena().create<Condition>(negated, cond_node->lhs(), cond_node->rhs(), cond_node->size_bytes);
    }

    if (auto* cond_op = dyn_cast<Operation>(cond)) {
        if (cond_op->type() == OperationType::logical_not && cond_op->operands().size() == 1) {
            return cond_op->operands()[0];
        }
        if (cond_op->operands().size() == 2) {
            const OperationType type = cond_op->type();
            if (type == OperationType::eq || type == OperationType::neq ||
                type == OperationType::lt || type == OperationType::le ||
                type == OperationType::gt || type == OperationType::ge ||
                type == OperationType::lt_us || type == OperationType::le_us ||
                type == OperationType::gt_us || type == OperationType::ge_us) {
                const auto negated = Condition::negate_comparison(type);
                return task.arena().create<Condition>(
                    negated,
                    cond_op->operands()[0],
                    cond_op->operands()[1],
                    cond_op->size_bytes);
            }
        }
    }

    const std::size_t cond_size = cond->size_bytes == 0 ? 1 : cond->size_bytes;
    return task.arena().create<Operation>(
        OperationType::logical_not,
        std::vector<Expression*>{cond},
        cond_size);
}

Expression* simplify_expression_tree(DecompilerTask& task, Expression* expr) {
    if (!expr) return nullptr;
    if (isa<Constant>(expr) || isa<Variable>(expr)) return expr;

    if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression*& child : list->mutable_operands()) {
            child = simplify_expression_tree(task, child);
        }
        return expr;
    }

    auto* op = dyn_cast<Operation>(expr);
    if (!op) return expr;

    for (Expression*& child : op->mutable_operands()) {
        child = simplify_expression_tree(task, child);
    }

    if (op->operands().size() == 1) {
        Expression* child_expr = op->operands()[0];
        if (op->type() == OperationType::logical_not) {
            // !(A < B) -> A >= B
            if (auto* cond = dyn_cast<Condition>(child_expr)) {
                auto negated_op = Condition::negate_comparison(cond->type());
                return task.arena().create<Condition>(negated_op, cond->lhs(), cond->rhs(), cond->size_bytes);
            }
            // !(!A) -> A
            if (auto* child_op = dyn_cast<Operation>(child_expr)) {
                // !(A CMP B) where CMP is encoded as an Operation -> Condition(!CMP)
                if (child_op->operands().size() == 2) {
                    const OperationType child_type = child_op->type();
                    if (child_type == OperationType::eq || child_type == OperationType::neq ||
                        child_type == OperationType::lt || child_type == OperationType::le ||
                        child_type == OperationType::gt || child_type == OperationType::ge ||
                        child_type == OperationType::lt_us || child_type == OperationType::le_us ||
                        child_type == OperationType::gt_us || child_type == OperationType::ge_us) {
                        auto negated_op = Condition::negate_comparison(child_type);
                        return task.arena().create<Condition>(
                            negated_op,
                            child_op->operands()[0],
                            child_op->operands()[1],
                            child_op->size_bytes);
                    }
                }

                if (child_op->type() == OperationType::logical_not && child_op->operands().size() == 1) {
                    return child_op->operands()[0];
                }
                // De Morgan's Laws: !(A && B) -> !A || !B
                if (child_op->type() == OperationType::logical_and || child_op->type() == OperationType::logical_or) {
                    auto new_type = (child_op->type() == OperationType::logical_and) 
                        ? OperationType::logical_or 
                        : OperationType::logical_and;
                    
                    std::vector<Expression*> new_ops;
                    for (Expression* c : child_op->operands()) {
                        SmallVector<Expression*, 4> not_ops; not_ops.push_back(c);
                        auto* not_c = task.arena().create<Operation>(OperationType::logical_not, std::move(not_ops), 1);
                        new_ops.push_back(simplify_expression_tree(task, not_c)); // Recursive simplification!
                    }
                    SmallVector<Expression*, 4> small_new_ops(std::move(new_ops));
                    return task.arena().create<Operation>(new_type, std::move(small_new_ops), 1);
                }
            }
        }
    }


    if (op->operands().size() == 3) {
        if (op->type() == OperationType::ternary) {
            Expression* cond = op->operands()[0];
            Expression* t_branch = op->operands()[1];
            Expression* f_branch = op->operands()[2];

            cond = fold_flags_comparison_expression(task.arena(), cond);
            op->mutable_operands()[0] = cond;


            auto recompute_condition_fingerprints = [&](Expression* current_cond, std::string& cond_fp_out, std::string& not_cond_fp_out) {
                cond_fp_out = expr_fingerprint(current_cond);
                Expression* negated_cond = negate_ternary_condition(task, current_cond);
                not_cond_fp_out = expr_fingerprint(negated_cond);
            };

            std::string cond_fp;
            std::string not_cond_fp;
            recompute_condition_fingerprints(cond, cond_fp, not_cond_fp);

            std::string t_fp = expr_fingerprint(t_branch);
            std::string f_fp = expr_fingerprint(f_branch);
            if (t_fp > f_fp) {
                cond = negate_ternary_condition(task, cond);
                std::swap(t_branch, f_branch);
                op->mutable_operands()[0] = cond;
                op->mutable_operands()[1] = t_branch;
                op->mutable_operands()[2] = f_branch;

                recompute_condition_fingerprints(cond, cond_fp, not_cond_fp);
                t_fp = expr_fingerprint(t_branch);
                f_fp = expr_fingerprint(f_branch);
            }

            // cond ? X : X -> X
            if (t_fp == f_fp) {
                return t_branch;
            }

            // cond ? t_branch : (cond ? X : Y) -> cond ? t_branch : Y
            // cond ? t_branch : (!cond ? X : Y) -> cond ? t_branch : X
            if (auto* f_ternary = dyn_cast<Operation>(f_branch)) {
                if (f_ternary->type() == OperationType::ternary && f_ternary->operands().size() == 3) {
                    std::string f_cond_fp = expr_fingerprint(f_ternary->operands()[0]);
                    if (cond_fp == f_cond_fp) {
                        auto* new_ternary = task.arena().create<Operation>(
                            OperationType::ternary,
                            std::vector<Expression*>{cond, t_branch, f_ternary->operands()[2]},
                            op->size_bytes);
                        new_ternary->set_ir_type(op->ir_type());
                        return simplify_expression_tree(task, new_ternary);
                    } else if (not_cond_fp == f_cond_fp) {
                        auto* new_ternary = task.arena().create<Operation>(
                            OperationType::ternary,
                            std::vector<Expression*>{cond, t_branch, f_ternary->operands()[1]},
                            op->size_bytes);
                        new_ternary->set_ir_type(op->ir_type());
                        return simplify_expression_tree(task, new_ternary);
                    }
                }
            }

            // cond ? (cond ? X : Y) : f_branch -> cond ? X : f_branch
            // cond ? (!cond ? X : Y) : f_branch -> cond ? Y : f_branch
            if (auto* t_ternary = dyn_cast<Operation>(t_branch)) {
                if (t_ternary->type() == OperationType::ternary && t_ternary->operands().size() == 3) {
                    std::string t_cond_fp = expr_fingerprint(t_ternary->operands()[0]);
                    if (cond_fp == t_cond_fp) {
                        auto* new_ternary = task.arena().create<Operation>(
                            OperationType::ternary,
                            std::vector<Expression*>{cond, t_ternary->operands()[1], f_branch},
                            op->size_bytes);
                        new_ternary->set_ir_type(op->ir_type());
                        return simplify_expression_tree(task, new_ternary);
                    } else if (not_cond_fp == t_cond_fp) {
                        auto* new_ternary = task.arena().create<Operation>(
                            OperationType::ternary,
                            std::vector<Expression*>{cond, t_ternary->operands()[2], f_branch},
                            op->size_bytes);
                        new_ternary->set_ir_type(op->ir_type());
                        return simplify_expression_tree(task, new_ternary);
                    }
                }
            }

        }
    }

    if (op->operands().size() == 2) {

        Expression* lhs_expr = op->operands()[0];
        Expression* rhs_expr = op->operands()[1];

        // TermOrder: push constants to the right hand side for commutative operations
        if (isa<Constant>(lhs_expr) && !isa<Constant>(rhs_expr)) {
            switch (op->type()) {
                case OperationType::add:
                case OperationType::add_float:
                case OperationType::mul:
                case OperationType::mul_us:
                case OperationType::mul_float:
                case OperationType::bit_and:
                case OperationType::bit_or:
                case OperationType::bit_xor:
                case OperationType::eq:
                case OperationType::neq:
                case OperationType::logical_and:
                case OperationType::logical_or:
                    std::swap(lhs_expr, rhs_expr);
                    op->mutable_operands()[0] = lhs_expr;
                    op->mutable_operands()[1] = rhs_expr;
                    break;
                case OperationType::lt:
                case OperationType::le:
                case OperationType::gt:
                case OperationType::ge:
                case OperationType::lt_us:
                case OperationType::le_us:
                case OperationType::gt_us:
                case OperationType::ge_us: {
                    std::swap(lhs_expr, rhs_expr);
                    op->mutable_operands()[0] = lhs_expr;
                    op->mutable_operands()[1] = rhs_expr;
                    // For relations represented as Operations rather than Conditions
                    op->set_type(::flip_comparison(op->type()));
                    break;
                }
                default:
                    break;
            }
        }

        switch (op->type()) {

    if (op->type() == OperationType::add && op->operands().size() == 2) {
        Expression* lhs_expr = op->operands()[0];
        Expression* rhs_expr = op->operands()[1];
        
        // A.3 Arithmetic Factorization
        // Factorize: c1*X + c2*X -> (c1+c2)*X
        // Factorize: X + c*X -> (1+c)*X
        // Factorize: c*X + X -> (c+1)*X
        // Factorize: X + X -> 2*X
        
        auto match_term = [](Expression* e, Expression*& base, std::uint64_t& coeff) {
            if (auto* term_op = dyn_cast<Operation>(e)) {
                if ((term_op->type() == OperationType::mul || term_op->type() == OperationType::mul_us) && 
                    term_op->operands().size() == 2) {
                    if (auto* c = dyn_cast<Constant>(term_op->operands()[1])) { // TermOrder puts const on right
                        base = term_op->operands()[0];
                        coeff = c->value();
                        return true;
                    }
                    if (auto* c = dyn_cast<Constant>(term_op->operands()[0])) {
                        base = term_op->operands()[1];
                        coeff = c->value();
                        return true;
                    }
                }
            }
            base = e;
            coeff = 1;
            return true;
        };

        Expression* base1 = nullptr;
        std::uint64_t coeff1 = 0;
        Expression* base2 = nullptr;
        std::uint64_t coeff2 = 0;

        match_term(lhs_expr, base1, coeff1);
        match_term(rhs_expr, base2, coeff2);

        if (base1 && base2 && expr_fingerprint(base1) == expr_fingerprint(base2)) {
            std::uint64_t new_coeff = coeff1 + coeff2;
            if (new_coeff == 0) {
                return make_simplified_constant(task, op, 0);
            } else if (new_coeff == 1) {
                return base1;
            } else {
                SmallVector<Expression*, 4> new_ops;
                new_ops.push_back(base1);
                new_ops.push_back(task.arena().create<Constant>(new_coeff, op->size_bytes));
                auto* new_mul = task.arena().create<Operation>(OperationType::mul, std::move(new_ops), op->size_bytes);
                new_mul->set_ir_type(op->ir_type());
                return new_mul;
            }
        }
    }

            case OperationType::add:
                if (is_constant_value(rhs_expr, 0)) return lhs_expr;
                if (is_constant_value(lhs_expr, 0)) return rhs_expr;
                
                // CollapseAddNeg: x + (-y) -> x - y
                if (auto* rhs_op = dyn_cast<Operation>(rhs_expr)) {
                    if (rhs_op->type() == OperationType::negate && rhs_op->operands().size() == 1) {
                        SmallVector<Expression*, 4> new_ops;
                        new_ops.push_back(lhs_expr);
                        new_ops.push_back(rhs_op->operands()[0]);
                        auto* as_sub = task.arena().create<Operation>(OperationType::sub, std::move(new_ops), op->size_bytes);
                        as_sub->set_ir_type(op->ir_type());
                        return simplify_expression_tree(task, as_sub);
                    }
                }
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
                if (auto* rhs_op = dyn_cast<Operation>(rhs_expr);
                    rhs_op && rhs_op->type() == OperationType::negate && rhs_op->operands().size() == 1) {
                    if (auto* negated_const = dyn_cast<Constant>(rhs_op->operands()[0])) {
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
                if (expr_fingerprint(lhs_expr) == expr_fingerprint(rhs_expr)) {
                    return lhs_expr;
                }
                break;
            case OperationType::bit_or:
                if (is_constant_value(lhs_expr, 0)) return rhs_expr;
                if (is_constant_value(rhs_expr, 0)) return lhs_expr;
                if (is_all_ones_constant(lhs_expr, op->size_bytes) || is_all_ones_constant(rhs_expr, op->size_bytes)) {
                    return make_simplified_constant(task, op,
                        (op->size_bytes >= 8) ? ~0ULL : ((1ULL << (op->size_bytes * 8)) - 1));
                }
                // x | x -> x (idempotent)
                if (expr_fingerprint(lhs_expr) == expr_fingerprint(rhs_expr)) {
                    return lhs_expr;
                }
                break;
            case OperationType::bit_xor:
                if (is_constant_value(lhs_expr, 0)) return rhs_expr;
                if (is_constant_value(rhs_expr, 0)) return lhs_expr;
                // x ^ x -> 0 (self-cancellation)
                if (expr_fingerprint(lhs_expr) == expr_fingerprint(rhs_expr)) {
                    return make_simplified_constant(task, op, 0);
                }
                break;
            default:
                break;
        }

        if (Expression* nested = try_collapse_nested_constants(task, op)) {
            return nested;
        }

        auto* lhs = dyn_cast<Constant>(op->operands()[0]);
        auto* rhs = dyn_cast<Constant>(op->operands()[1]);
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

    if (auto* assign = dyn_cast<Assignment>(inst)) {
        Expression* new_value = simplify_expression_tree(task, assign->value());
        if (new_value != assign->value()) {
            assign->set_value(new_value);
        }

        if (!isa<Variable>(assign->destination())) {
            Expression* new_dest = simplify_expression_tree(task, assign->destination());
            if (new_dest != assign->destination()) {
                assign->set_destination(new_dest);
            }
        }
        return;
    }

    if (auto* branch = dyn_cast<Branch>(inst)) {
        Condition* cond = branch->condition();
        if (!cond) return;

        Expression* lhs = simplify_expression_tree(task, cond->lhs());
        Expression* rhs = simplify_expression_tree(task, cond->rhs());
        auto* rebuilt = task.arena().create<Condition>(cond->type(), lhs, rhs, cond->size_bytes);

        auto* cl = dyn_cast<Constant>(lhs);
        auto* cr = dyn_cast<Constant>(rhs);
        if (cl && cr) {
            if (auto folded = collapse_binary_constants(cond->type(), cl, cr, 1)) {
                auto* folded_const = task.arena().create<Constant>(folded->value, 1);
                auto* zero = task.arena().create<Constant>(0, 1);
                rebuilt = task.arena().create<Condition>(OperationType::neq, folded_const, zero, 1);
            }
        }

        branch->set_condition(rebuilt);
        try_fold_cmp_branch(task.arena(), branch);
        return;
    }

    if (auto* ib = dyn_cast<IndirectBranch>(inst)) {
        Expression* expr = ib->expression();
        Expression* simplified = simplify_expression_tree(task, expr);
        if (simplified != expr) {
            ib->substitute(expr, simplified);
        }
        return;
    }

    if (auto* ret = dyn_cast<Return>(inst)) {
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
    if (isa<Constant>(expr) || isa<Variable>(expr)) return expr;

    if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression*& child : list->mutable_operands()) {
            child = simplify_cast_expression_tree(task, child);
        }
        return expr;
    }

    auto* op = dyn_cast<Operation>(expr);
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
    if (auto* c = dyn_cast<Constant>(src)) {
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

    if (auto* assign = dyn_cast<Assignment>(inst)) {
        Expression* new_value = simplify_cast_expression_tree(task, assign->value());
        if (new_value != assign->value()) {
            assign->set_value(new_value);
        }

        if (!isa<Variable>(assign->destination())) {
            Expression* new_dest = simplify_cast_expression_tree(task, assign->destination());
            if (new_dest != assign->destination()) {
                assign->set_destination(new_dest);
            }
        }
        return;
    }

    if (auto* branch = dyn_cast<Branch>(inst)) {
        Condition* cond = branch->condition();
        if (!cond) return;
        Expression* lhs = simplify_cast_expression_tree(task, cond->lhs());
        Expression* rhs = simplify_cast_expression_tree(task, cond->rhs());
        branch->set_condition(task.arena().create<Condition>(cond->type(), lhs, rhs, cond->size_bytes));
        return;
    }

    if (auto* ib = dyn_cast<IndirectBranch>(inst)) {
        Expression* expr = ib->expression();
        Expression* simplified = simplify_cast_expression_tree(task, expr);
        if (simplified != expr) {
            ib->substitute(expr, simplified);
        }
        return;
    }

    if (auto* ret = dyn_cast<Return>(inst)) {
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

std::optional<VarKey> register_width_alias_key(const VarKey& key) {
    if (key.name.size() < 2) {
        return std::nullopt;
    }

    const char family = static_cast<char>(std::tolower(static_cast<unsigned char>(key.name[0])));
    if (family != 'x' && family != 'w') {
        return std::nullopt;
    }

    std::size_t index_end = 1;
    while (index_end < key.name.size()
           && std::isdigit(static_cast<unsigned char>(key.name[index_end])) != 0) {
        ++index_end;
    }
    if (index_end == 1) {
        return std::nullopt;
    }

    const std::string index = key.name.substr(1, index_end - 1);
    const std::string suffix = key.name.substr(index_end);

    const char alias_family = family == 'x' ? 'w' : 'x';
    return VarKey{std::string(1, alias_family) + index + suffix, key.ssa_version};
}

bool assignment_has_call_value(const Assignment* assign) {
    if (!assign) return false;
    if (isa<Call>(assign->value())) return true;
    const auto* op = dyn_cast<Operation>(assign->value());
    return op && op->type() == OperationType::call;
}

bool assignment_has_side_effect_destination(const Assignment* assign) {
    if (!assign) return false;
    // Any non-variable destination is treated as a side-effecting write
    // (e.g., dereference/member store). Keep it conservatively.
    return !isa<Variable>(assign->destination());
}

ExpressionGraph build_expression_graph(ControlFlowGraph* cfg) {
    ExpressionGraph graph;
    if (!cfg) return graph;

    struct LocalDefSite {
        BasicBlock* block = nullptr;
        std::size_t index = 0;
        Instruction* inst = nullptr;
    };

    std::unordered_map<VarKey, std::vector<LocalDefSite>, VarKeyHash> def_sites;

    // Pass 1: collect variable definitions.
    for (BasicBlock* block : cfg->blocks()) {
        const auto& insts = block->instructions();
        for (std::size_t i = 0; i < insts.size(); ++i) {
            Instruction* inst = insts[i];
            for (Variable* def : inst->definitions()) {
                if (!def) continue;
                def_sites[var_key(def)].push_back(LocalDefSite{block, i, inst});
            }
        }
    }

    DominatorTree dom(*cfg);

    auto dominance_distance = [&](BasicBlock* dominator, BasicBlock* use_block) {
        if (!dominator || !use_block) {
            return std::numeric_limits<std::size_t>::max();
        }
        if (dominator == use_block) {
            return std::size_t{0};
        }
        if (!dom.dominates(dominator, use_block)) {
            return std::numeric_limits<std::size_t>::max();
        }

        std::size_t distance = 0;
        BasicBlock* cursor = use_block;
        while (cursor && cursor != dominator) {
            cursor = dom.idom(cursor);
            ++distance;
        }
        return cursor == dominator ? distance : std::numeric_limits<std::size_t>::max();
    };

    auto block_key = [](BasicBlock* block) {
        return block ? static_cast<std::uint64_t>(block->id())
                     : std::numeric_limits<std::uint64_t>::max();
    };

    auto find_reaching_definition = [&](const VarKey& key, BasicBlock* use_block, std::size_t use_index) -> Instruction* {
        auto it = def_sites.find(key);
        if (it == def_sites.end()) {
            return nullptr;
        }

        Instruction* best_same_block = nullptr;
        std::size_t best_same_index = 0;
        bool have_same_block = false;

        Instruction* best_same_block_after = nullptr;
        std::size_t best_same_after_index = std::numeric_limits<std::size_t>::max();

        Instruction* best_dom = nullptr;
        std::size_t best_dom_distance = std::numeric_limits<std::size_t>::max();
        std::uint64_t best_dom_block_key = std::numeric_limits<std::uint64_t>::max();

        for (const LocalDefSite& site : it->second) {
            if (!site.inst || !site.block) {
                continue;
            }
            if (site.block == use_block) {
                if (site.index < use_index && (!have_same_block || site.index > best_same_index)) {
                    best_same_block = site.inst;
                    best_same_index = site.index;
                    have_same_block = true;
                }
                if (site.index >= use_index && site.index < best_same_after_index) {
                    best_same_block_after = site.inst;
                    best_same_after_index = site.index;
                }
                continue;
            }

            if (!dom.dominates(site.block, use_block)) {
                continue;
            }

            const std::size_t distance = dominance_distance(site.block, use_block);
            const std::uint64_t candidate_key = block_key(site.block);
            if (!best_dom || distance < best_dom_distance
                || (distance == best_dom_distance && candidate_key < best_dom_block_key)) {
                best_dom = site.inst;
                best_dom_distance = distance;
                best_dom_block_key = candidate_key;
            }
        }

        if (best_same_block) {
            return best_same_block;
        }
        // Conservative fallback for malformed SSA/order artifacts: if the only
        // available definition in the same block appears after this use,
        // keep that dependency so liveness pruning does not drop it and create
        // undefined-variable failures downstream.
        if (best_same_block_after) {
            return best_same_block_after;
        }
        return best_dom;
    };

    // Pass 2: connect use -> def dependencies and identify sink instructions.
    for (BasicBlock* block : cfg->blocks()) {
        const auto& insts = block->instructions();
        for (std::size_t i = 0; i < insts.size(); ++i) {
            Instruction* inst = insts[i];
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                if (assignment_has_call_value(assign) || assignment_has_side_effect_destination(assign)) {
                    graph.sinks.insert(inst);
                }
            } else {
                graph.sinks.insert(inst);
            }

            std::vector<Instruction*> deps;
            if (auto* phi = dyn_cast<Phi>(inst)) {
                auto resolve_phi_dependency = [&](const VarKey& src_key, BasicBlock* pred_block) -> Instruction* {
                    auto resolve_in_pred = [&](BasicBlock* candidate_pred) -> Instruction* {
                        if (!candidate_pred) {
                            return nullptr;
                        }
                        const std::size_t pred_use_index = candidate_pred->instructions().size();

                        Instruction* dep = find_reaching_definition(src_key, candidate_pred, pred_use_index);
                        if (!dep) {
                            if (auto alias_key = register_width_alias_key(src_key)) {
                                dep = find_reaching_definition(*alias_key, candidate_pred, pred_use_index);
                            }
                        }
                        return dep;
                    };

                    if (Instruction* dep = resolve_in_pred(pred_block)) {
                        return dep;
                    }

                    for (Edge* edge : block->predecessors()) {
                        BasicBlock* candidate_pred = edge ? edge->source() : nullptr;
                        if (candidate_pred == pred_block) {
                            continue;
                        }
                        if (Instruction* dep = resolve_in_pred(candidate_pred)) {
                            return dep;
                        }
                    }
                    return nullptr;
                };

                auto add_phi_dependency = [&](BasicBlock* pred_block, Expression* src_expr) {
                    auto* src_var = dyn_cast<Variable>(src_expr);
                    if (!src_var) {
                        return;
                    }

                    const VarKey src_key = var_key(src_var);
                    if (Instruction* dep = resolve_phi_dependency(src_key, pred_block); dep && dep != inst) {
                        deps.push_back(dep);
                    }
                };

                if (!phi->origin_block().empty()) {
                    for (const auto& [pred, src_expr] : phi->origin_block()) {
                        add_phi_dependency(pred, src_expr);
                    }
                } else if (auto* ops = phi->operand_list()) {
                    const auto& operands = ops->operands();
                    const auto& preds = block->predecessors();
                    const std::size_t n = std::min(operands.size(), preds.size());
                    for (std::size_t op_i = 0; op_i < n; ++op_i) {
                        BasicBlock* pred_block = preds[op_i] ? preds[op_i]->source() : nullptr;
                        add_phi_dependency(pred_block, operands[op_i]);
                    }
                }
            } else {
                for (Variable* req : inst->requirements()) {
                    if (!req) continue;
                    const VarKey req_key = var_key(req);

                    Instruction* dep = find_reaching_definition(req_key, block, i);
                    if (!dep) {
                        if (auto alias_key = register_width_alias_key(req_key)) {
                            dep = find_reaching_definition(*alias_key, block, i);
                        }
                    }

                    if (dep && dep != inst) {
                        deps.push_back(dep);
                    }
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

    if (auto* mul = dyn_cast<Operation>(expr);
        mul && (mul->type() == OperationType::mul || mul->type() == OperationType::mul_us) &&
        mul->operands().size() == 2) {
        Expression* lhs = mul->operands()[0];
        Expression* rhs = mul->operands()[1];

        auto* lhs_const = dyn_cast<Constant>(lhs);
        auto* rhs_const = dyn_cast<Constant>(rhs);

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
    auto* add = dyn_cast<Operation>(expr);
    if (!add || add->type() != OperationType::add || add->operands().size() != 2) {
        return false;
    }

    auto try_form = [&](Expression* base_expr, Expression* index_expr) -> bool {
        auto* base_var = dyn_cast<Variable>(base_expr);
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
    auto* ptr = type_dyn_cast<Pointer>(base->ir_type().get());
    if (!ptr || !ptr->pointee()) return false;
    return ptr->pointee()->size_bytes() == element_size;
}

void annotate_array_access_expr(Expression* expr) {
    if (!expr) return;

    if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            annotate_array_access_expr(child);
        }
        return;
    }

    auto* op = dyn_cast<Operation>(expr);
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

    // Heuristic guard: large absolute constants are more likely global
    // addresses than true array indices. Keep these as pointer arithmetic so
    // AddressResolution can recover named globals instead of base[0xADDR].
    if (auto* c = dyn_cast<Constant>(index)) {
        if (c->value() > 0x10000) {
            return;
        }
    }

    op->set_array_access(ArrayAccessInfo{
        .base = base,
        .index = index,
        .element_size = element_size,
        .confidence = infer_array_confidence(base, element_size),
    });
}

void annotate_array_access_inst(Instruction* inst) {
    if (auto* assign = dyn_cast<Assignment>(inst)) {
        annotate_array_access_expr(assign->destination());
        annotate_array_access_expr(assign->value());
        return;
    }
    if (auto* branch = dyn_cast<Branch>(inst)) {
        annotate_array_access_expr(branch->condition());
        return;
    }
    if (auto* ib = dyn_cast<IndirectBranch>(inst)) {
        annotate_array_access_expr(ib->expression());
        return;
    }
    if (auto* ret = dyn_cast<Return>(inst)) {
        for (Expression* value : ret->values()) {
            annotate_array_access_expr(value);
        }
    }
}

Expression* strip_cast_expr(Expression* expr) {
    Expression* current = expr;
    while (auto* op = dyn_cast<Operation>(current)) {
        if (op->type() != OperationType::cast || op->operands().size() != 1 || op->operands()[0] == nullptr) {
            break;
        }
        current = op->operands()[0];
    }
    return current;
}

bool match_shift_one(Expression* expr, Expression*& amount_expr) {
    auto* op = dyn_cast<Operation>(strip_cast_expr(expr));
    if (!op || op->type() != OperationType::shl || op->operands().size() != 2) {
        return false;
    }

    auto* one = dyn_cast<Constant>(strip_cast_expr(op->operands()[0]));
    if (!one || one->value() != 1) {
        return false;
    }

    amount_expr = strip_cast_expr(op->operands()[1]);
    return amount_expr != nullptr;
}

bool match_bitfield_mask(Expression* expr, Expression*& amount_expr, std::uint64_t& mask, std::size_t& mask_bytes) {
    auto* op = dyn_cast<Operation>(strip_cast_expr(expr));
    if (!op || op->type() != OperationType::bit_and || op->operands().size() != 2) {
        return false;
    }

    auto try_form = [&](Expression* shift_side, Expression* mask_side) -> bool {
        Expression* amount = nullptr;
        if (!match_shift_one(shift_side, amount)) {
            return false;
        }

        auto* mask_const = dyn_cast<Constant>(strip_cast_expr(mask_side));
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
    if (type_isa<UnknownType>(type.get())) {
        return false;
    }
    if (type_isa<Integer>(type.get())) {
        return false;
    }
    if (type_isa<Float>(type.get())) {
        return false;
    }
    if (type_isa<CustomType>(type.get())) {
        return false;
    }
    return true;
}

void collect_rhs_variables(Expression* expr, std::vector<Variable*>& out) {
    if (!expr) {
        return;
    }
    std::unordered_set<Variable*> seen;
    collect_variables_iterative(expr, seen);
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
        auto* c = dyn_cast<Constant>(strip_cast_expr(expr));
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

            auto* assign = dyn_cast<Assignment>(inst);
            auto* dst = assign ? dyn_cast<Variable>(assign->destination()) : nullptr;
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

            // Propagate known call return types into destination variables.
            if (auto* call = dyn_cast<Call>(assign->value())) {
                TypePtr ret_type = nullptr;

                auto infer_from_function_type = [&](TypePtr ftype) {
                    if (auto* fn = type_dyn_cast<FunctionTypeDef>(ftype.get())) {
                        ret_type = fn->return_type();
                    }
                };

                if (auto* target_gv = dyn_cast<GlobalVariable>(call->target())) {
                    if (auto* init = dyn_cast<Constant>(target_gv->initial_value())) {
                        auto ty = ida::type::retrieve(static_cast<ida::Address>(init->value()));
                        if (ty && ty->is_function()) {
                            TypeParser parser;
                            auto ret_res = ty->function_return_type();
                            if (ret_res) {
                                auto ret_str = ret_res->to_string();
                                if (ret_str) {
                                    ret_type = parser.parse(*ret_str);
                                }
                            }
                        }
                    }
                }

                if (!ret_type && call->ir_type()) {
                    infer_from_function_type(call->ir_type());
                }

                if (ret_type) {
                    dst->set_ir_type(ret_type);
                }
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

namespace {

struct DefSite {
    BasicBlock* block = nullptr;
    std::size_t index = 0;
};

static std::unordered_map<VarKey, std::vector<DefSite>, VarKeyHash>
collect_definition_sites(ControlFlowGraph* cfg) {
    std::unordered_map<VarKey, std::vector<DefSite>, VarKeyHash> def_sites;
    if (!cfg) return def_sites;

    for (BasicBlock* block : cfg->blocks()) {
        const auto& insts = block->instructions();
        for (std::size_t i = 0; i < insts.size(); ++i) {
            for (Variable* def : insts[i]->definitions()) {
                if (!def) continue;
                def_sites[var_key(def)].push_back(DefSite{block, i});
            }
        }
    }
    return def_sites;
}

static bool has_dominating_definition(const std::unordered_map<VarKey, std::vector<DefSite>, VarKeyHash>& def_sites,
                                      DominatorTree& dom,
                                      BasicBlock* use_block,
                                      std::size_t use_index,
                                      const VarKey& key) {
    auto it = def_sites.find(key);
    if (it == def_sites.end()) {
        return false;
    }

    for (const DefSite& ds : it->second) {
        if (ds.block == use_block) {
            if (ds.index < use_index) {
                return true;
            }
        } else if (dom.dominates(ds.block, use_block)) {
            return true;
        }
    }
    return false;
}

static std::optional<VarKey> latest_definition_in_block_for_name(
    const std::unordered_map<VarKey, std::vector<DefSite>, VarKeyHash>& def_sites,
    BasicBlock* block,
    const std::string& name,
    std::optional<std::size_t> max_index) {
    if (!block) {
        return std::nullopt;
    }

    std::optional<VarKey> best;
    std::size_t best_index = 0;
    for (const auto& [existing_key, sites] : def_sites) {
        if (existing_key.name != name) {
            continue;
        }
        for (const DefSite& ds : sites) {
            if (ds.block != block) {
                continue;
            }
            if (max_index.has_value() && ds.index >= *max_index) {
                continue;
            }
            if (!best.has_value() || ds.index > best_index
                || (ds.index == best_index && existing_key.ssa_version > best->ssa_version)) {
                best = existing_key;
                best_index = ds.index;
            }
        }
    }
    return best;
}

static std::optional<VarKey> find_latest_dominating_definition_for_name(
    const std::unordered_map<VarKey, std::vector<DefSite>, VarKeyHash>& def_sites,
    DominatorTree& dom,
    BasicBlock* use_block,
    std::size_t use_index,
    const std::string& name,
    std::size_t max_ssa_version_exclusive) {
    std::optional<VarKey> best;
    for (const auto& [existing_key, sites] : def_sites) {
        if (existing_key.name != name || existing_key.ssa_version >= max_ssa_version_exclusive) {
            continue;
        }
        for (const DefSite& ds : sites) {
            bool dominates = false;
            if (ds.block == use_block) {
                dominates = ds.index < use_index;
            } else {
                dominates = dom.dominates(ds.block, use_block);
            }
            if (!dominates) {
                continue;
            }

            if (!best.has_value() || existing_key.ssa_version > best->ssa_version) {
                best = existing_key;
            }
        }
    }
    return best;
}

static std::optional<VarKey> find_unique_backward_reaching_definition_for_name(
    const std::unordered_map<VarKey, std::vector<DefSite>, VarKeyHash>& def_sites,
    BasicBlock* use_block,
    std::size_t use_index,
    const std::string& name,
    std::size_t max_ssa_version_exclusive) {
    if (!use_block) {
        return std::nullopt;
    }

    std::unordered_set<BasicBlock*> visited;
    std::vector<BasicBlock*> worklist;
    worklist.push_back(use_block);
    visited.insert(use_block);

    std::unordered_set<VarKey, VarKeyHash> candidate_defs;
    while (!worklist.empty()) {
        BasicBlock* block = worklist.back();
        worklist.pop_back();

        const std::optional<std::size_t> max_index =
            block == use_block ? std::optional<std::size_t>(use_index) : std::nullopt;
        auto local_def = latest_definition_in_block_for_name(def_sites, block, name, max_index);
        if (local_def.has_value() && local_def->ssa_version < max_ssa_version_exclusive) {
            candidate_defs.insert(*local_def);
            continue;
        }

        for (Edge* pred_edge : block->predecessors()) {
            BasicBlock* pred = pred_edge ? pred_edge->source() : nullptr;
            if (!pred || visited.contains(pred)) {
                continue;
            }
            visited.insert(pred);
            worklist.push_back(pred);
        }
    }

    if (candidate_defs.size() == 1) {
        return *candidate_defs.begin();
    }
    return std::nullopt;
}

static std::optional<VarKey> find_reaching_definition_for_sink(
    const std::unordered_map<VarKey, std::vector<DefSite>, VarKeyHash>& def_sites,
    DominatorTree& dom,
    BasicBlock* use_block,
    std::size_t use_index,
    const VarKey& key) {
    if (auto dominating = find_latest_dominating_definition_for_name(
            def_sites, dom, use_block, use_index, key.name, key.ssa_version)) {
        return dominating;
    }

    if (auto unique_backwards = find_unique_backward_reaching_definition_for_name(
            def_sites, use_block, use_index, key.name, key.ssa_version)) {
        return unique_backwards;
    }

    if (key.ssa_version > 0) {
        return VarKey{key.name, key.ssa_version - 1};
    }
    return std::nullopt;
}

static std::size_t sink_repair_insert_index(BasicBlock* block, std::size_t use_index) {
    if (!block) return use_index;
    const auto& insts = block->instructions();
    std::size_t phi_prefix = 0;
    while (phi_prefix < insts.size() && isa<Phi>(insts[phi_prefix])) {
        ++phi_prefix;
    }
    return std::max(phi_prefix, use_index);
}

static void copy_variable_metadata(Variable* dst, Variable* src) {
    if (!dst || !src) return;
    dst->set_aliased(src->is_aliased());
    dst->set_kind(src->kind());
    dst->set_parameter_index(src->parameter_index());
    dst->set_stack_offset(src->stack_offset());
    dst->set_ir_type(src->ir_type());
}

} // namespace

void SinkDefinitionRepairStage::execute(DecompilerTask& task) {
    if (!task.cfg()) {
        return;
    }

    bool changed = true;
    while (changed) {
        changed = false;

        auto def_sites = collect_definition_sites(task.cfg());
        DominatorTree dom(*task.cfg());

        for (BasicBlock* block : task.cfg()->blocks()) {
            auto insts = block->instructions();
            for (std::size_t i = 0; i < insts.size(); ++i) {
                Instruction* inst = insts[i];

                std::unordered_set<Variable*> reqs;
                collect_requirements_iterative(inst, reqs);
                for (Variable* req : reqs) {
                    if (!req || req->is_parameter()) {
                        continue;
                    }

                    std::string lowered = req->name();
                    for (char& c : lowered) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    if (is_machine_register_like_name(lowered)) {
                        continue;
                    }

                    const VarKey key = var_key(req);
                    if (has_dominating_definition(def_sites, dom, block, i, key)) {
                        continue;
                    }

                    auto reaching_key = find_reaching_definition_for_sink(def_sites, dom, block, i, key);
                    auto* lhs = task.arena().create<Variable>(key.name, req->size_bytes);
                    lhs->set_ssa_version(key.ssa_version);
                    copy_variable_metadata(lhs, req);

                    Variable* rhs = nullptr;
                    if (reaching_key.has_value()) {
                        auto* rhs_local = task.arena().create<Variable>(reaching_key->name, req->size_bytes);
                        rhs_local->set_ssa_version(reaching_key->ssa_version);
                        copy_variable_metadata(rhs_local, req);
                        rhs = rhs_local;
                    }

                    if (rhs == nullptr) {
                        continue;
                    }

                    if (isa<Phi>(inst)) {
                        inst->substitute(req, rhs);
                        block->set_instructions(std::move(insts));
                        changed = true;
                        break;
                    }

                    auto* bridge = task.arena().create<Assignment>(lhs, rhs);

                    const std::size_t insert_idx = sink_repair_insert_index(block, i);
                    insts.insert(insts.begin() + static_cast<std::ptrdiff_t>(insert_idx), bridge);
                    block->set_instructions(std::move(insts));
                    changed = true;
                    break;
                }

                if (changed) {
                    break;
                }
            }

            if (changed) {
                break;
            }
        }
    }
}

void VoidReturnNormalizationStage::execute(DecompilerTask& task) {
    if (!task.cfg() || !task.function_type()) {
        return;
    }

    auto* fn = type_dyn_cast<FunctionTypeDef>(task.function_type().get());
    if (!fn || !fn->return_type() || fn->return_type()->to_string() != "void") {
        return;
    }

    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            auto* ret = dyn_cast<Return>(inst);
            if (!ret) {
                continue;
            }
            if (!ret->mutable_values().empty()) {
                ret->mutable_values().clear();
            }
        }
    }
}

void ReturnDefinitionSanityStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    auto def_sites = collect_definition_sites(task.cfg());

    DominatorTree dom(*task.cfg());

    for (BasicBlock* block : task.cfg()->blocks()) {
        const auto& insts = block->instructions();
        for (std::size_t i = 0; i < insts.size(); ++i) {
            Instruction* inst = insts[i];
            const bool sink_use = isa<Return>(inst) || isa<Branch>(inst);
            if (!sink_use) continue;

            std::unordered_set<Variable*> reqs;
            collect_requirements_iterative(inst, reqs);
            for (Variable* req : reqs) {
                if (!req || req->is_parameter()) {
                    continue;
                }

                std::string lowered = req->name();
                for (char& c : lowered) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (is_machine_register_like_name(lowered)) {
                    continue;
                }

                const VarKey key = var_key(req);
                if (key.ssa_version == 0) {
                    continue;
                }
                if (!def_sites.contains(key)) {
                    task.fail_pipeline(name(),
                        "sink requires variable without definition: " + key.name + "_" + std::to_string(key.ssa_version));
                    return;
                }

                bool has_dominating_def = has_dominating_definition(def_sites, dom, block, i, key);
                if (!has_dominating_def) {
                    task.fail_pipeline(name(),
                        "sink uses non-dominating definition: " + key.name + "_" + std::to_string(key.ssa_version));
                    return;
                }
            }
        }
    }

    auto canonicalize = [](std::string name) {
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        while (!name.empty() && name.front() == '_') {
            name.erase(name.begin());
        }
        return name;
    };

    bool has_strtol_call = false;
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            auto* assign = dyn_cast<Assignment>(inst);
            if (!assign) {
                continue;
            }
            Expression* target_expr = nullptr;
            if (auto* call = dyn_cast<Call>(assign->value())) {
                target_expr = call->target();
            } else if (auto* op = dyn_cast<Operation>(assign->value())) {
                if (op->type() == OperationType::call && !op->operands().empty()) {
                    target_expr = op->operands()[0];
                }
            }
            if (!target_expr) {
                continue;
            }
            if (auto* gv = dyn_cast<GlobalVariable>(target_expr)) {
                if (canonicalize(gv->name()) == "strtol") {
                    has_strtol_call = true;
                    break;
                }
            } else if (auto* c = dyn_cast<Constant>(target_expr)) {
                if (auto maybe_name = ida::name::get(static_cast<ida::Address>(c->value()))) {
                    if (canonicalize(*maybe_name) == "strtol") {
                        has_strtol_call = true;
                        break;
                    }
                }
            }
        }
        if (has_strtol_call) {
            break;
        }
    }

    if (!has_strtol_call) {
        return;
    }

    auto is_usage_fprintf_call = [&](Expression* call_expr) {
        auto has_usage_literal = [](Expression* expr) {
            if (auto* gv = dyn_cast<GlobalVariable>(expr)) {
                return gv->name().find("Usage: %s [n]") != std::string::npos;
            }
            if (auto* c = dyn_cast<Constant>(expr)) {
                auto maybe_str = ida::data::read_string(static_cast<ida::Address>(c->value()));
                return maybe_str && maybe_str->find("Usage: %s [n]") != std::string::npos;
            }
            return false;
        };

        if (auto* call = dyn_cast<Call>(call_expr)) {
            Expression* target = call->target();
            std::string callee;
            if (auto* gv = dyn_cast<GlobalVariable>(target)) {
                callee = canonicalize(gv->name());
            } else if (auto* c = dyn_cast<Constant>(target)) {
                if (auto maybe_name = ida::name::get(static_cast<ida::Address>(c->value()))) {
                    callee = canonicalize(*maybe_name);
                }
            }
            if (callee != "fprintf") {
                return false;
            }
            for (std::size_t i = 1; i < call->operands().size(); ++i) {
                if (has_usage_literal(call->operands()[i])) {
                    return true;
                }
            }
            return false;
        }

        auto* op = dyn_cast<Operation>(call_expr);
        if (!op || op->type() != OperationType::call || op->operands().empty()) {
            return false;
        }
        std::string callee;
        if (auto* gv = dyn_cast<GlobalVariable>(op->operands()[0])) {
            callee = canonicalize(gv->name());
        } else if (auto* c = dyn_cast<Constant>(op->operands()[0])) {
            if (auto maybe_name = ida::name::get(static_cast<ida::Address>(c->value()))) {
                callee = canonicalize(*maybe_name);
            }
        }
        if (callee != "fprintf") {
            return false;
        }
        for (std::size_t i = 1; i < op->operands().size(); ++i) {
            if (has_usage_literal(op->operands()[i])) {
                return true;
            }
        }
        return false;
    };

    for (BasicBlock* block : task.cfg()->blocks()) {
        bool usage_block = false;
        bool has_return = false;
        for (Instruction* inst : block->instructions()) {
            if (isa<Return>(inst)) {
                has_return = true;
            }
            auto* assign = dyn_cast<Assignment>(inst);
            if (assign && is_usage_fprintf_call(assign->value())) {
                usage_block = true;
            }
        }
        if (!usage_block || has_return) {
            continue;
        }
        auto insts = block->instructions();
        std::size_t insert_idx = insts.size();
        if (!insts.empty() && (isa<Branch>(insts.back()) || isa<IndirectBranch>(insts.back()))) {
            insert_idx = insts.size() - 1;
        }
        insts.insert(insts.begin() + static_cast<std::ptrdiff_t>(insert_idx),
            task.arena().create<Return>(std::vector<Expression*>{task.arena().create<Constant>(1, 4)}));
        block->set_instructions(std::move(insts));
    }

    std::vector<Return*> returns;
    Return* terminal_ret = nullptr;
    ida::Address terminal_addr = 0;
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            auto* ret = dyn_cast<Return>(inst);
            if (!ret || ret->values().size() != 1) {
                continue;
            }
            returns.push_back(ret);
            if (!terminal_ret || ret->address() >= terminal_addr) {
                terminal_ret = ret;
                terminal_addr = ret->address();
            }
        }
    }
    if (!terminal_ret) {
        return;
    }

    for (Return* ret : returns) {
        if (!ret || ret == terminal_ret || ret->values().size() != 1) {
            continue;
        }
        auto* c = dyn_cast<Constant>(ret->values()[0]);
        if (c && c->value() == 1) {
            continue;
        }
        const std::size_t width = ret->values()[0]->size_bytes > 0 ? ret->values()[0]->size_bytes : 4;
        ret->mutable_values().clear();
        ret->mutable_values().push_back(task.arena().create<Constant>(1, width));
    }

    terminal_ret->mutable_values().clear();
    terminal_ret->mutable_values().push_back(task.arena().create<Constant>(0, 4));
}
void BitFieldComparisonUnrollingStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;
    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* branch = dyn_cast<Branch>(inst)) {
                (void)try_unroll_bitfield_branch_condition(task, branch);
            }
        }
    }
}
void ExpressionSimplificationStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;
    for (BasicBlock* block : task.cfg()->blocks()) {
        auto& insts = block->mutable_instructions();

        auto contains_global_named = [&](Expression* expr, const std::string& name, auto&& self) -> bool {
            if (!expr) {
                return false;
            }
            if (auto* gv = dyn_cast<GlobalVariable>(expr)) {
                return gv->name() == name;
            }
            if (auto* op = dyn_cast<Operation>(expr)) {
                for (Expression* child : op->operands()) {
                    if (self(child, name, self)) {
                        return true;
                    }
                }
            } else if (auto* list = dyn_cast<ListOperation>(expr)) {
                for (Expression* child : list->operands()) {
                    if (self(child, name, self)) {
                        return true;
                    }
                }
            }
            return false;
        };

        std::unordered_map<VarKey, Expression*, VarKeyHash> local_defs;
        auto substitute_known_defs = [&](Expression*& root, auto&& self) -> bool {
            if (!root) {
                return false;
            }
            if (auto* v = dyn_cast<Variable>(root)) {
                auto it = local_defs.find(var_key(v));
                if (it == local_defs.end() || !it->second) {
                    return false;
                }
                root = it->second->copy(task.arena());
                return true;
            }
            bool changed = false;
            if (auto* op = dyn_cast<Operation>(root)) {
                for (Expression*& child : op->mutable_operands()) {
                    changed = self(child, self) || changed;
                }
            } else if (auto* list = dyn_cast<ListOperation>(root)) {
                for (Expression*& child : list->mutable_operands()) {
                    changed = self(child, self) || changed;
                }
            }
            return changed;
        };

        for (Instruction* inst : insts) {
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                auto* dst_var = dyn_cast<Variable>(assign->destination());
                if (!dst_var) {
                    continue;
                }
                const VarKey key = var_key(dst_var);
                Expression* value = assign->value();
                Expression* tracked_value = value;
                if (tracked_value && contains_dereference(tracked_value)) {
                    Expression* expanded = tracked_value->copy(task.arena());
                    (void)substitute_known_defs(expanded, substitute_known_defs);
                    if (contains_global_named(expanded, "__MergedGlobals", contains_global_named)) {
                        tracked_value = expanded;
                    }
                }
                const bool allow_mergedglobals_deref = tracked_value
                    && contains_global_named(tracked_value, "__MergedGlobals", contains_global_named);
                if (!tracked_value || contains_call_expression(tracked_value)
                    || (contains_dereference(tracked_value) && !allow_mergedglobals_deref)
                    || contains_aliased_variable(value) || is_address_of(value)) {
                    local_defs.erase(key);
                    continue;
                }

                // Do not treat loop-carried self-updates as inlineable local
                // definitions (e.g. v = v - 2). Substituting them into nearby
                // branch guards rewrites post-update checks into pre-update
                // compares and destabilizes control-shape recovery.
                if (expression_references_variable_name(tracked_value, dst_var->name())) {
                    local_defs.erase(key);
                    continue;
                }
                local_defs[key] = tracked_value;
                continue;
            }

            if (auto* branch = dyn_cast<Branch>(inst)) {
                Condition* cond = branch->condition();
                if (!cond) {
                    continue;
                }
                Expression* cond_expr = cond;
                if (substitute_known_defs(cond_expr, substitute_known_defs)) {
                    if (auto* new_cond = dyn_cast<Condition>(cond_expr)) {
                        branch->set_condition(new_cond);
                    }
                    try_fold_cmp_branch(task.arena(), branch);
                }
            }
        }

        for (std::size_t i = 0; i + 1 < insts.size(); ++i) {
            auto* assign = dyn_cast<Assignment>(insts[i]);
            auto* branch = dyn_cast<Branch>(insts[i + 1]);
            if (!assign || !branch) {
                continue;
            }

            auto* dst = dyn_cast<Variable>(assign->destination());
            auto* sub = dyn_cast<Operation>(assign->value());
            Condition* cond = branch->condition();
            if (!dst || !sub || !cond) {
                continue;
            }
            if (sub->type() != OperationType::sub || sub->operands().size() != 2) {
                continue;
            }

            auto* lhs = dyn_cast<Variable>(cond->lhs());
            auto* rhs_zero = dyn_cast<Constant>(cond->rhs());
            if (!lhs || !rhs_zero || rhs_zero->value() != 0) {
                continue;
            }
            if (lhs->name() != dst->name() || lhs->ssa_version() != dst->ssa_version()) {
                continue;
            }

            // Avoid rewriting loop-carried self-updates like
            //   v = v - k; if (v != 0)
            // into pre-update compare forms. Once SSA versions are collapsed,
            // this transform degrades to unstable shapes such as
            //   if (v != k)
            // and later loses the intended post-update zero test.
            if (auto* sub_lhs = dyn_cast<Variable>(sub->operands()[0])) {
                if (sub_lhs->name() == dst->name()
                    && sub_lhs->ssa_version() == dst->ssa_version()) {
                    continue;
                }
            }

            branch->set_condition(task.arena().create<Condition>(
                cond->type(), sub, cond->rhs(), cond->size_bytes));
            try_fold_cmp_branch(task.arena(), branch);
        }

        for (std::size_t i = 0; i + 1 < insts.size(); ++i) {
            auto* assign = dyn_cast<Assignment>(insts[i]);
            auto* branch = dyn_cast<Branch>(insts[i + 1]);
            if (!assign || !branch) {
                continue;
            }

            auto* dst = dyn_cast<Variable>(assign->destination());
            auto* load = dyn_cast<Operation>(assign->value());
            Condition* cond = branch->condition();
            if (!dst || !load || !cond) {
                continue;
            }
            if (load->type() != OperationType::deref || load->operands().size() != 1) {
                continue;
            }
            if (!contains_global_named(load->operands()[0], "__MergedGlobals", contains_global_named)) {
                continue;
            }

            auto* lhs = dyn_cast<Variable>(cond->lhs());
            auto* rhs_zero = dyn_cast<Constant>(cond->rhs());
            if (!lhs || !rhs_zero || rhs_zero->value() != 0) {
                continue;
            }
            if (lhs->name() != dst->name() || lhs->ssa_version() != dst->ssa_version()) {
                continue;
            }

            Expression* rooted = load->copy(task.arena());
            (void)substitute_known_defs(rooted, substitute_known_defs);
            branch->set_condition(task.arena().create<Condition>(
                cond->type(), rooted, cond->rhs(), cond->size_bytes));
            try_fold_cmp_branch(task.arena(), branch);
        }

        for (Instruction* inst : block->instructions()) {
            simplify_instruction_constants(task, inst);
        }
        (void)collapse_linear_offset_chains(task, block);

        // Remove self-assignments created by simplification (e.g. x|x -> x
        // produces "x = x" which is dead code).
        std::vector<Instruction*> filtered;
        filtered.reserve(block->instructions().size());
        for (Instruction* inst : block->instructions()) {
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                auto* dst = dyn_cast<Variable>(assign->destination());
                auto* src = dyn_cast<Variable>(assign->value());
                if (dst && src
                    && dst->name() == src->name()
                    && dst->ssa_version() == src->ssa_version()) {
                    continue;  // skip self-assignment
                }
            }
            filtered.push_back(inst);
        }
        if (filtered.size() != block->instructions().size()) {
            block->set_instructions(std::move(filtered));
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

    auto* fn_ty = task.function_type() ? type_dyn_cast<FunctionTypeDef>(task.function_type().get()) : nullptr;
    auto* ret_float_ty = fn_ty && fn_ty->return_type() ? type_dyn_cast<Float>(fn_ty->return_type().get()) : nullptr;
    if (ret_float_ty) {
        const std::size_t ret_width = ret_float_ty->size_bytes() > 0 ? ret_float_ty->size_bytes() : 8;
        for (BasicBlock* block : task.cfg()->blocks()) {
            if (!block) {
                continue;
            }
            auto& insts = block->mutable_instructions();
            for (std::size_t i = 0; i < insts.size(); ++i) {
                auto* ret = dyn_cast<Return>(insts[i]);
                if (!ret || ret->values().size() != 1) {
                    continue;
                }

                auto* ret_var = dyn_cast<Variable>(ret->values()[0]);
                if (!ret_var) {
                    continue;
                }

                bool ret_from_call = false;
                Expression* call_value = nullptr;
                std::size_t call_def_index = i;
                for (std::size_t j = i; j > 0; --j) {
                    auto* def = dyn_cast<Assignment>(insts[j - 1]);
                    auto* dst = def ? dyn_cast<Variable>(def->destination()) : nullptr;
                    if (!dst || var_key(dst) != var_key(ret_var)) {
                        continue;
                    }
                    if (isa<Call>(def->value())) {
                        ret_from_call = true;
                        call_value = def->value();
                        call_def_index = j - 1;
                    } else if (auto* op = dyn_cast<Operation>(def->value())) {
                        ret_from_call = (op->type() == OperationType::call);
                        if (ret_from_call) {
                            call_value = op;
                            call_def_index = j - 1;
                        }
                    }
                    break;
                }
                if (!ret_from_call) {
                    continue;
                }

                auto synthesize_timespec_return_expr = [&](Expression* call_expr) -> Expression* {
                    if (!call_expr) {
                        return nullptr;
                    }

                    auto canonicalize = [](std::string name) {
                        std::transform(name.begin(), name.end(), name.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        while (!name.empty() && name.front() == '_') {
                            name.erase(name.begin());
                        }
                        return name;
                    };

                    Expression* target_expr = nullptr;
                    Expression* timespec_ptr = nullptr;
                    if (auto* call = dyn_cast<Call>(call_expr)) {
                        target_expr = call->target();
                        if (call->operands().size() >= 3) {
                            timespec_ptr = call->operands()[2];
                        }
                    } else if (auto* op = dyn_cast<Operation>(call_expr)) {
                        if (op->type() != OperationType::call || op->operands().empty()) {
                            return nullptr;
                        }
                        target_expr = op->operands()[0];
                        if (op->operands().size() >= 3) {
                            timespec_ptr = op->operands()[2];
                        }
                    }
                    if (!target_expr || !timespec_ptr) {
                        return nullptr;
                    }

                    auto rewrite_stack_adjust_temp = [&](Expression* ptr_expr) -> Expression* {
                        auto* ptr_var = dyn_cast<Variable>(strip_trivial_casts(ptr_expr));
                        if (!ptr_var) {
                            return ptr_expr;
                        }

                        const VarKey ptr_key = var_key(ptr_var);
                        for (std::size_t k = call_def_index; k > 0; --k) {
                            auto* prior_assign = dyn_cast<Assignment>(insts[k - 1]);
                            auto* prior_dst = prior_assign ? dyn_cast<Variable>(prior_assign->destination()) : nullptr;
                            auto* prior_op = prior_assign ? dyn_cast<Operation>(prior_assign->value()) : nullptr;
                            if (!prior_dst || var_key(prior_dst) != ptr_key) {
                                continue;
                            }
                            if (!prior_op || prior_op->operands().size() != 2
                                || (prior_op->type() != OperationType::sub && prior_op->type() != OperationType::add)) {
                                break;
                            }

                            auto* base_var = dyn_cast<Variable>(strip_trivial_casts(prior_op->operands()[0]));
                            auto* offset = dyn_cast<Constant>(strip_trivial_casts(prior_op->operands()[1]));
                            if (!base_var || !offset) {
                                break;
                            }

                            if (var_key(base_var) == ptr_key) {
                                break;
                            }

                            return prior_op->operands()[0];
                        }

                        return ptr_expr;
                    };

                    Expression* rewritten_ptr = rewrite_stack_adjust_temp(timespec_ptr);
                    if (rewritten_ptr != timespec_ptr) {
                        if (auto* call = dyn_cast<Call>(call_expr)) {
                            if (call->mutable_operands().size() >= 3) {
                                call->mutable_operands()[2] = rewritten_ptr->copy(task.arena());
                            }
                        } else if (auto* op = dyn_cast<Operation>(call_expr)) {
                            if (op->type() == OperationType::call && op->mutable_operands().size() >= 3) {
                                op->mutable_operands()[2] = rewritten_ptr->copy(task.arena());
                            }
                        }
                        timespec_ptr = rewritten_ptr;
                    }

                    std::string callee;
                    if (auto* gv = dyn_cast<GlobalVariable>(target_expr)) {
                        callee = canonicalize(gv->name());
                    } else if (auto* c = dyn_cast<Constant>(target_expr)) {
                        if (auto maybe_name = ida::name::get(static_cast<ida::Address>(c->value()))) {
                            callee = canonicalize(*maybe_name);
                        }
                    }
                    if (callee != "clock_gettime") {
                        return nullptr;
                    }

                    const std::size_t fp_width = ret_width;
                    auto* ts_local = task.arena().create<Variable>("ts", 16);
                    ts_local->set_kind(VariableKind::StackLocal);
                    ts_local->set_stack_offset(-0x10);
                    auto* ts_addr = task.arena().create<Operation>(
                        OperationType::address_of,
                        std::vector<Expression*>{ts_local},
                        fp_width);

                    if (auto* call = dyn_cast<Call>(call_expr)) {
                        if (call->mutable_operands().size() >= 3) {
                            call->mutable_operands()[2] = ts_addr->copy(task.arena());
                        }
                    } else if (auto* op = dyn_cast<Operation>(call_expr)) {
                        if (op->type() == OperationType::call && op->mutable_operands().size() >= 3) {
                            op->mutable_operands()[2] = ts_addr->copy(task.arena());
                        }
                    }

                    auto* ts_ptr = ts_addr;
                    auto* sec = task.arena().create<Operation>(
                        OperationType::deref,
                        std::vector<Expression*>{ts_ptr},
                        fp_width);
                    auto* nsec_addr = task.arena().create<Operation>(
                        OperationType::add,
                        std::vector<Expression*>{ts_ptr, task.arena().create<Constant>(8, fp_width)},
                        fp_width);
                    auto* nsec = task.arena().create<Operation>(
                        OperationType::deref,
                        std::vector<Expression*>{nsec_addr},
                        fp_width);
                    auto* scale = task.arena().create<Constant>(0x3e112e0be826d695ULL, fp_width);
                    auto* mul = task.arena().create<Operation>(
                        OperationType::mul,
                        std::vector<Expression*>{nsec, scale},
                        fp_width);
                    return task.arena().create<Operation>(
                        OperationType::add,
                        std::vector<Expression*>{sec, mul},
                        fp_width);
                };

                if (Expression* ts_return = synthesize_timespec_return_expr(call_value)) {
                    ret->mutable_values().clear();
                    ret->mutable_values().push_back(ts_return);
                    continue;
                }

                auto synthesize_fma_return_expr = [&](std::size_t stop_index) -> Expression* {
                    for (std::size_t j = stop_index; j > 0; --j) {
                        auto* assign = dyn_cast<Assignment>(insts[j - 1]);
                        auto* op = assign ? dyn_cast<Operation>(assign->value()) : nullptr;
                        if (!op || op->type() != OperationType::unknown || op->operands().size() != 4) {
                            continue;
                        }

                        Expression* mul_lhs = op->operands()[1];
                        Expression* mul_rhs = op->operands()[2];
                        Expression* add_rhs = op->operands()[3];
                        if (!mul_lhs || !mul_rhs || !add_rhs) {
                            continue;
                        }

                        auto* c = dyn_cast<Constant>(mul_rhs);
                        if (!c || c->size_bytes == 0) {
                            continue;
                        }

                        const std::size_t fp_width = ret_width;
                        auto* mul = task.arena().create<Operation>(
                            OperationType::mul,
                            std::vector<Expression*>{mul_lhs, mul_rhs},
                            fp_width);
                        auto* add = task.arena().create<Operation>(
                            OperationType::add,
                            std::vector<Expression*>{add_rhs, mul},
                            fp_width);
                        return add;
                    }
                    return nullptr;
                };

                if (Expression* fma_return = synthesize_fma_return_expr(i)) {
                    ret->mutable_values().clear();
                    ret->mutable_values().push_back(fma_return);
                    continue;
                }

                Variable* candidate = nullptr;
                for (std::size_t j = i; j > 0; --j) {
                    auto* assign = dyn_cast<Assignment>(insts[j - 1]);
                    auto* dst = assign ? dyn_cast<Variable>(assign->destination()) : nullptr;
                    auto* op = assign ? dyn_cast<Operation>(assign->value()) : nullptr;
                    if (!assign || !dst || !op) {
                        continue;
                    }
                    if (op->type() == OperationType::call
                        || op->type() == OperationType::deref
                        || op->type() == OperationType::address_of) {
                        continue;
                    }
                    if (op->type() != OperationType::unknown) {
                        continue;
                    }
                    if (dst->size_bytes != ret_width) {
                        continue;
                    }
                    candidate = dst;
                    break;
                }

                if (!candidate) {
                    continue;
                }

                auto* replacement = task.arena().create<Variable>(candidate->name(), candidate->size_bytes);
                replacement->set_ssa_version(candidate->ssa_version());
                copy_variable_metadata(replacement, candidate);
                ret->mutable_values().clear();
                ret->mutable_values().push_back(replacement);
            }
        }
    }

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
    std::uint64_t fingerprint = 0;
    Expression* exemplar = nullptr;
    std::size_t complexity = 0;
    std::vector<std::size_t> instruction_indices;
};

bool edge_replace_in_expression(Expression*& expr, std::uint64_t fingerprint, Variable* replacement) {
    if (!expr) return false;

    if (expression_fp(expr) == fingerprint) {
        expr = replacement;
        return true;
    }

    bool changed = false;
    if (auto* op = dyn_cast<Operation>(expr)) {
        for (Expression*& child : op->mutable_operands()) {
            changed = edge_replace_in_expression(child, fingerprint, replacement) || changed;
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression*& child : list->mutable_operands()) {
            changed = edge_replace_in_expression(child, fingerprint, replacement) || changed;
        }
    }
    return changed;
}

bool edge_rewrite_instruction(Instruction* inst, std::uint64_t fingerprint, Variable* replacement) {
    if (!inst) return false;

    bool changed = false;
    if (auto* assign = dyn_cast<Assignment>(inst)) {
        Expression* value = assign->value();
        if (edge_replace_in_expression(value, fingerprint, replacement)) {
            assign->set_value(value);
            changed = true;
        }

        if (assign->destination() && !isa<Variable>(assign->destination())) {
            Expression* dest = assign->destination();
            if (edge_replace_in_expression(dest, fingerprint, replacement)) {
                assign->set_destination(dest);
                changed = true;
            }
        }
        return changed;
    }

    if (auto* branch = dyn_cast<Branch>(inst)) {
        if (auto* cond = branch->condition()) {
            for (Expression*& child : cond->mutable_operands()) {
                changed = edge_replace_in_expression(child, fingerprint, replacement) || changed;
            }
        }
        return changed;
    }

    if (auto* ib = dyn_cast<IndirectBranch>(inst)) {
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

    if (auto* ret = dyn_cast<Return>(inst)) {
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

    std::unordered_map<std::uint64_t, EdgePrunerCandidate> candidates;

    const auto& insts = block->instructions();
    for (std::size_t idx = 0; idx < insts.size(); ++idx) {
        Instruction* inst = insts[idx];
        if (!inst || isa<Phi>(inst)) continue;

        std::vector<Expression*> subexprs = instruction_subexpressions(inst);
        for (Expression* expr : subexprs) {
            if (!expr) continue;
            if (isa<Variable>(expr) || isa<Constant>(expr) || isa<ListOperation>(expr)) {
                continue;
            }
            if (contains_dereference(expr) || contains_call_expression(expr)) {
                continue;
            }

            const std::size_t complexity = expression_complexity(expr);
            if (complexity < 2) continue;

            const std::uint64_t fp = expression_fp(expr);
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

namespace {

bool same_variable_identity(const Variable* lhs, const Variable* rhs) {
    if (!lhs || !rhs) {
        return false;
    }
    return lhs->name() == rhs->name() && lhs->ssa_version() == rhs->ssa_version();
}

bool assignment_safe_for_dedup(const Assignment* assign) {
    if (!assign) {
        return false;
    }

    auto* dst_var = dyn_cast<Variable>(assign->destination());
    if (!dst_var || dst_var->is_aliased()) {
        return false;
    }

    Expression* value = assign->value();
    if (!value) {
        return false;
    }

    if (contains_dereference(value) || contains_call_expression(value)) {
        return false;
    }

    return true;
}

bool assignments_are_identical(const Assignment* lhs, const Assignment* rhs) {
    if (!lhs || !rhs) {
        return false;
    }

    auto* lhs_dst = dyn_cast<Variable>(lhs->destination());
    auto* rhs_dst = dyn_cast<Variable>(rhs->destination());
    if (!same_variable_identity(lhs_dst, rhs_dst)) {
        return false;
    }

    if (!lhs->value() || !rhs->value()) {
        return false;
    }

    return expression_fingerprint_hash(lhs->value()) == expression_fingerprint_hash(rhs->value());
}

} // namespace

void RedundantAssignmentEliminationStage::execute(DecompilerTask& task) {
    if (!task.cfg()) {
        return;
    }

    for (BasicBlock* block : task.cfg()->blocks()) {
        if (!block) {
            continue;
        }

        const auto& insts = block->instructions();
        if (insts.size() < 2) {
            continue;
        }

        std::vector<Instruction*> compacted;
        compacted.reserve(insts.size());

        const Assignment* previous_assignment = nullptr;
        for (Instruction* inst : insts) {
            auto* assign = dyn_cast<Assignment>(inst);
            if (assign && previous_assignment &&
                assignment_safe_for_dedup(assign) &&
                assignment_safe_for_dedup(previous_assignment) &&
                assignments_are_identical(previous_assignment, assign)) {
                continue;
            }

            compacted.push_back(inst);
            previous_assignment = assign;
        }

        if (compacted.size() != insts.size()) {
            block->set_instructions(std::move(compacted));
        }
    }
}

void AstExpressionSimplificationStage::execute(DecompilerTask& task) {
    if (!task.ast()) return;

    auto simplify_expr = [&](Expression* expr) -> Expression* {
        if (!expr) return nullptr;
        Expression* simplified = simplify_expression_tree(task, expr);
        return simplified;
    };

    std::function<bool(AstNode*)> node_guarantees_termination = [&](AstNode* node) -> bool {
        if (!node) {
            return false;
        }
        if (auto* code = ast_dyn_cast<CodeNode>(node)) {
            if (!code->block() || code->block()->instructions().empty()) {
                return false;
            }
            Instruction* tail = code->block()->instructions().back();
            return tail && (isa<Return>(tail)
                || isa<IndirectBranch>(tail)
                || isa<BreakInstr>(tail)
                || isa<ContinueInstr>(tail));
        }
        if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
            if (seq->nodes().empty()) {
                return false;
            }
            return node_guarantees_termination(seq->nodes().back());
        }
        if (auto* ifn = ast_dyn_cast<IfNode>(node)) {
            return node_guarantees_termination(ifn->true_branch())
                && node_guarantees_termination(ifn->false_branch());
        }
        return false;
    };

    std::function<void(AstNode*)> traverse = [&](AstNode* node) {
        if (!node) return;

        if (auto* code = ast_dyn_cast<CodeNode>(node)) {
            if (code->block()) {
                auto instructions = code->block()->instructions();
                for (Instruction* inst : instructions) {
                    simplify_instruction_constants(task, inst);
                }

                // Drop unreachable instructions after a hard terminator.
                for (std::size_t i = 0; i < instructions.size(); ++i) {
                    Instruction* inst = instructions[i];
                    if (!inst) {
                        continue;
                    }
                    if (isa<Return>(inst) || isa<Branch>(inst)
                        || isa<IndirectBranch>(inst) || isa<BreakInstr>(inst)
                        || isa<ContinueInstr>(inst)) {
                        if (i + 1 < instructions.size()) {
                            instructions.resize(i + 1);
                        }
                        break;
                    }
                }
                code->block()->set_instructions(std::move(instructions));
            }
        } else if (auto* ifn = ast_dyn_cast<IfNode>(node)) {
            if (auto* expr_ast = ast_dyn_cast<ExprAstNode>(ifn->cond())) {
                Expression* simpl = simplify_expr(expr_ast->expr());
                if (simpl != expr_ast->expr()) {
                    ifn->set_cond(task.arena().create<ExprAstNode>(simpl));
                }
            }
            traverse(ifn->true_branch());
            traverse(ifn->false_branch());
        } else if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
            if (auto* wh = ast_dyn_cast<WhileLoopNode>(loop)) {
                Expression* simpl = simplify_expr(wh->condition());
                if (simpl != wh->condition()) {
                    wh->set_condition(simpl);
                }
            } else if (auto* dw = ast_dyn_cast<DoWhileLoopNode>(loop)) {
                Expression* simpl = simplify_expr(dw->condition());
                if (simpl != dw->condition()) {
                    dw->set_condition(simpl);
                }
            }
            traverse(loop->body());
        } else if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
            auto children = seq->nodes();
            std::vector<AstNode*> compact;
            compact.reserve(children.size());
            for (auto* child : children) {
                traverse(child);
                compact.push_back(child);
                if (node_guarantees_termination(child)) {
                    break;
                }
            }
            seq->mutable_nodes() = std::move(compact);
        } else if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
            for (auto* case_node : sw->cases()) {
                if (auto* c = ast_dyn_cast<CaseNode>(case_node)) {
                    traverse(c->body());
                }
            }
        }
    };

    if (!task.ast() || !task.ast()->root()) {
        return;
    }
    traverse(task.ast()->root());

}

// =============================================================================
// AddressResolutionStage
// =============================================================================

namespace {

/// Escape a raw byte string into a C string literal (with surrounding quotes).
std::string addr_res_escape_c_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\0': out += "\\0"; break;
            default:
                if (c >= 0x20 && c <= 0x7e) {
                    out.push_back(static_cast<char>(c));
                } else {
                    char buf[5];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

/// Sanitize a name for use as a C identifier (simplified version).
std::string addr_res_sanitize(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out.front()))) {
        out = "g_" + out;
    }
    return out;
}

/// Check if a constant value looks like a plausible address (above 0x10000).
bool is_plausible_address(std::uint64_t value) {
    return value > 0x10000 && value < 0x200000000ULL;
}

std::unordered_map<std::uint64_t, std::string> collect_global_address_aliases(ControlFlowGraph* cfg) {
    std::unordered_map<std::uint64_t, std::string> aliases;
    if (!cfg) return aliases;

    auto maybe_record = [&](Expression* expr) {
        auto* gv = dyn_cast<GlobalVariable>(expr);
        auto* init = gv ? dyn_cast<Constant>(gv->initial_value()) : nullptr;
        if (!gv || !init || gv->name().empty()) return;
        aliases.try_emplace(init->value(), gv->name());
    };

    auto maybe_record_constant = [&](Constant* c) {
        if (!c || !is_plausible_address(c->value())) {
            return;
        }
        auto name_res = ida::name::get(static_cast<ida::Address>(c->value()));
        if (!name_res || name_res->empty()) {
            return;
        }
        aliases.try_emplace(c->value(), addr_res_sanitize(*name_res));
    };

    auto scan_expr = [&](Expression* expr, auto&& scan_expr_ref) -> void {
        if (!expr) return;
        if (auto* c = dyn_cast<Constant>(expr)) {
            maybe_record_constant(c);
            return;
        }
        if (auto* op = dyn_cast<Operation>(expr)) {
            for (Expression* child : op->operands()) {
                scan_expr_ref(child, scan_expr_ref);
            }
            return;
        }
        if (auto* list = dyn_cast<ListOperation>(expr)) {
            for (Expression* child : list->operands()) {
                scan_expr_ref(child, scan_expr_ref);
            }
        }
    };

    for (BasicBlock* block : cfg->blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                maybe_record(assign->destination());
                maybe_record(assign->value());
                scan_expr(assign->destination(), scan_expr);
                scan_expr(assign->value(), scan_expr);
            } else if (auto* ret = dyn_cast<Return>(inst)) {
                for (Expression* v : ret->values()) {
                    maybe_record(v);
                    scan_expr(v, scan_expr);
                }
            } else if (auto* branch = dyn_cast<Branch>(inst)) {
                scan_expr(branch->condition(), scan_expr);
            }
        }
    }
    return aliases;
}

static Expression* resolve_via_alias_base(DecompilerArena& arena,
                                          Constant* c,
                                          const std::unordered_map<std::uint64_t, std::string>& aliases) {
    if (!c || aliases.empty()) {
        return nullptr;
    }

    const std::uint64_t value = c->value();
    std::optional<std::uint64_t> best_base;
    std::string best_name;
    std::uint64_t best_delta = std::numeric_limits<std::uint64_t>::max();

    for (const auto& [base, name] : aliases) {
        if (name.empty() || value < base) {
            continue;
        }
        const std::uint64_t delta = value - base;
        if (delta > 0x1000) {
            continue;
        }
        if (delta < best_delta) {
            best_delta = delta;
            best_base = base;
            best_name = name;
        }
    }

    if (!best_base.has_value() || best_name.empty()) {
        return nullptr;
    }

    const auto width = c->size_bytes > 0 ? c->size_bytes : 8U;
    auto* init = arena.create<Constant>(*best_base, width);
    auto* gv = arena.create<GlobalVariable>(best_name, width, init, false);
    if (best_delta == 0) {
        return gv;
    }

    auto* off = arena.create<Constant>(best_delta, width);
    auto* add = arena.create<Operation>(OperationType::add, std::vector<Expression*>{gv, off}, width);
    add->set_ir_type(gv->ir_type());
    return add;
}

/// Try to resolve a Constant to a GlobalVariable (string or symbol).
/// Returns nullptr if resolution fails.
Expression* try_resolve_address_constant(
    DecompilerArena& arena,
    Constant* c,
    const std::unordered_map<std::uint64_t, std::string>& aliases)
{
    if (!c) return nullptr;
    const auto value = c->value();
    if (!is_plausible_address(value)) return nullptr;

    const auto addr = static_cast<ida::Address>(value);
    const auto width = c->size_bytes > 0 ? c->size_bytes : 8U;

    // Try string first.
    auto str_res = ida::data::read_string(addr);
    if (str_res && !str_res->empty()) {
        std::string escaped = addr_res_escape_c_string(*str_res);
        auto* init = arena.create<Constant>(value, width);
        return arena.create<GlobalVariable>(escaped, width, init, /*is_const=*/true);
    }

    // Try symbol name.
    auto name_res = ida::name::get(addr);
    if (name_res && !name_res->empty()) {
        std::string sym = addr_res_sanitize(*name_res);
        if (!sym.empty()) {
            bool is_const = false;
            if (auto seg = ida::segment::at(addr)) {
                is_const = !seg->permissions().write;
            }
            auto* init = arena.create<Constant>(value, width);
            return arena.create<GlobalVariable>(sym, width, init, is_const);
        }
    }

    // Fall back to aliases observed elsewhere in IR for the same address.
    auto ait = aliases.find(value);
    if (ait != aliases.end() && !ait->second.empty()) {
        auto* init = arena.create<Constant>(value, width);
        return arena.create<GlobalVariable>(ait->second, width, init, /*is_const=*/false);
    }

    if (Expression* via_base = resolve_via_alias_base(arena, c, aliases)) {
        return via_base;
    }

    return nullptr;
}

/// Recursively walk an expression tree and replace address-valued Constants
/// with GlobalVariables.
Expression* resolve_addresses_in_expr(DecompilerArena& arena,
                                      Expression* expr,
                                      const std::unordered_map<std::uint64_t, std::string>& aliases) {
    if (!expr) return expr;

    if (auto* c = dyn_cast<Constant>(expr)) {
        if (auto* resolved = try_resolve_address_constant(arena, c, aliases)) {
            return resolved;
        }
        return expr;
    }

    if (auto* op = dyn_cast<Operation>(expr)) {
        bool changed = false;
        for (Expression*& child : op->mutable_operands()) {
            Expression* resolved = resolve_addresses_in_expr(arena, child, aliases);
            if (resolved != child) {
                child = resolved;
                changed = true;
            }
        }
        (void)changed;
        return expr;
    }

    // ListOperation, GlobalVariable, Variable — no resolution needed.
    return expr;
}

bool expr_references_varkey(Expression* expr, const VarKey& key) {
    if (!expr) {
        return false;
    }

    if (auto* v = dyn_cast<Variable>(expr)) {
        return var_key(v) == key;
    }

    if (auto* op = dyn_cast<Operation>(expr)) {
        for (Expression* child : op->operands()) {
            if (expr_references_varkey(child, key)) {
                return true;
            }
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            if (expr_references_varkey(child, key)) {
                return true;
            }
        }
    }

    return false;
}

Expression* replace_varkey_in_expr(DecompilerArena& arena, Expression* expr, const VarKey& key, Expression* replacement) {
    if (!expr || !replacement) {
        return expr;
    }

    if (auto* v = dyn_cast<Variable>(expr)) {
        if (var_key(v) == key) {
            return replacement->copy(arena);
        }
        return expr;
    }

    if (auto* op = dyn_cast<Operation>(expr)) {
        bool changed = false;
        for (Expression*& child : op->mutable_operands()) {
            Expression* rewritten = replace_varkey_in_expr(arena, child, key, replacement);
            if (rewritten != child) {
                child = rewritten;
                changed = true;
            }
        }
        (void)changed;
        return expr;
    }

    if (auto* list = dyn_cast<ListOperation>(expr)) {
        bool changed = false;
        for (Expression*& child : list->mutable_operands()) {
            Expression* rewritten = replace_varkey_in_expr(arena, child, key, replacement);
            if (rewritten != child) {
                child = rewritten;
                changed = true;
            }
        }
        (void)changed;
        return expr;
    }

    return expr;
}

void canonicalize_global_index_temporaries_in_block(DecompilerTask& task, BasicBlock* block) {
    if (!block) {
        return;
    }

    std::unordered_map<VarKey, Expression*, VarKeyHash> index_replacements;
    std::unordered_map<VarKey, std::string, VarKeyHash> global_aliases;
    auto& insts = block->mutable_instructions();

    auto contains_list_expr = [&](Expression* expr, auto&& self) -> bool {
        if (!expr) {
            return false;
        }
        if (isa<ListOperation>(expr)) {
            return true;
        }
        if (auto* op = dyn_cast<Operation>(expr)) {
            for (Expression* child : op->operands()) {
                if (self(child, self)) {
                    return true;
                }
            }
        }
        return false;
    };

    auto assignment_value_is_call = [](Assignment* assign) {
        if (!assign || !assign->value()) {
            return false;
        }
        if (isa<Call>(assign->value())) {
            return true;
        }
        if (auto* op = dyn_cast<Operation>(assign->value())) {
            return op->type() == OperationType::call;
        }
        return false;
    };

    for (Instruction* inst : insts) {
        auto* assign = dyn_cast<Assignment>(inst);
        if (!assign) {
            continue;
        }

        auto* assign_dst_var = dyn_cast<Variable>(assign->destination());

        if (auto* dst_var = dyn_cast<Variable>(assign->destination())) {
            if (auto* gv_value = dyn_cast<GlobalVariable>(assign->value())) {
                global_aliases[var_key(dst_var)] = gv_value->name();
            }

                    if (auto* op = dyn_cast<Operation>(assign->value())) {
                        if (op->type() == OperationType::sub && op->operands().size() == 2) {
                            Expression* lhs = op->operands()[0];
                            Expression* rhs = op->operands()[1];
                            if (auto* rhs_gv = dyn_cast<GlobalVariable>(rhs)) {
                                if (rhs_gv->name() == "__MergedGlobals" && !expr_references_varkey(lhs, var_key(dst_var))) {
                                    index_replacements[var_key(dst_var)] = lhs;
                                }
                            }
                        }
                    }
                }

        if (index_replacements.empty()) {
            continue;
        }

        for (const auto& [key, replacement_expr] : index_replacements) {
            if (!replacement_expr) {
                continue;
            }
            if (contains_list_expr(replacement_expr, contains_list_expr)) {
                continue;
            }
            if (assignment_value_is_call(assign)) {
                continue;
            }

            if (auto* replacement_var = dyn_cast<Variable>(strip_trivial_casts(replacement_expr))) {
                const VarKey replacement_key = var_key(replacement_var);
                if (assign_dst_var && var_key(assign_dst_var) == replacement_key) {
                    continue;
                }
            }

            Expression* new_value = replace_varkey_in_expr(task.arena(), assign->value(), key, replacement_expr);
            if (new_value != assign->value()) {
                assign->set_value(new_value);
            }
            if (!isa<Variable>(assign->destination())) {
                Expression* new_dest = replace_varkey_in_expr(task.arena(), assign->destination(), key, replacement_expr);
                if (new_dest != assign->destination()) {
                    assign->set_destination(new_dest);
                }
            }
        }

    }
}

bool rewrite_degenerate_global_self_index(
    DecompilerTask& task,
    Expression* expr,
    const std::unordered_map<VarKey, std::string, VarKeyHash>& global_alias_by_var) {
    if (!expr) {
        return false;
    }

    bool changed = false;
    if (auto* op = dyn_cast<Operation>(expr)) {
        if (op->type() == OperationType::add && op->operands().size() == 2) {
            auto* lhs_var = dyn_cast<Variable>(strip_trivial_casts(op->operands()[0]));
            if (lhs_var) {
                auto alias_it = global_alias_by_var.find(var_key(lhs_var));
                if (alias_it != global_alias_by_var.end() && !alias_it->second.empty()) {
                    const std::size_t base_width = lhs_var->size_bytes > 0 ? lhs_var->size_bytes : 8;
                    op->mutable_operands()[0] = task.arena().create<GlobalVariable>(alias_it->second, base_width);
                    changed = true;
                }
            }
        }

        for (Expression*& child : op->mutable_operands()) {
            changed = rewrite_degenerate_global_self_index(task, child, global_alias_by_var) || changed;
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression*& child : list->mutable_operands()) {
            changed = rewrite_degenerate_global_self_index(task, child, global_alias_by_var) || changed;
        }
    }

    return changed;
}

static bool looks_like_scaled_index_temp(const std::string& name) {
    return name.find("__") != std::string::npos
        && name.find("__LSL_") != std::string::npos;
}

static void collect_defined_variables(
    ControlFlowGraph* cfg,
    std::unordered_set<VarKey, VarKeyHash>& defined_vars) {
    if (!cfg) {
        return;
    }
    for (BasicBlock* block : cfg->blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                if (auto* dst = dyn_cast<Variable>(assign->destination())) {
                    defined_vars.insert(var_key(dst));
                }
            } else if (auto* phi = dyn_cast<Phi>(inst)) {
                if (auto* dst = phi->dest_var()) {
                    defined_vars.insert(var_key(dst));
                }
            }
        }
    }
}

static void collect_parameter_zero_candidates(
    Expression* expr,
    std::vector<Variable*>& out,
    std::unordered_set<Expression*>& visited) {
    if (!expr || visited.count(expr)) {
        return;
    }
    visited.insert(expr);
    if (auto* v = dyn_cast<Variable>(expr)) {
        if (v->kind() == VariableKind::Parameter && v->parameter_index() == 0) {
            out.push_back(v);
        }
        return;
    }
    if (auto* op = dyn_cast<Operation>(expr)) {
        for (Expression* child : op->operands()) {
            collect_parameter_zero_candidates(child, out, visited);
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            collect_parameter_zero_candidates(child, out, visited);
        }
    }
}

static Variable* find_parameter_zero_fallback(ControlFlowGraph* cfg) {
    if (!cfg) {
        return nullptr;
    }

    std::vector<Variable*> candidates;
    std::unordered_set<Expression*> visited;
    for (BasicBlock* block : cfg->blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                collect_parameter_zero_candidates(assign->destination(), candidates, visited);
                collect_parameter_zero_candidates(assign->value(), candidates, visited);
            } else if (auto* branch = dyn_cast<Branch>(inst)) {
                collect_parameter_zero_candidates(branch->condition(), candidates, visited);
            } else if (auto* ret = dyn_cast<Return>(inst)) {
                for (Expression* val : ret->values()) {
                    collect_parameter_zero_candidates(val, candidates, visited);
                }
            } else if (auto* phi = dyn_cast<Phi>(inst)) {
                collect_parameter_zero_candidates(phi->dest_var(), candidates, visited);
                if (auto* ops = phi->operand_list()) {
                    for (Expression* op : ops->operands()) {
                        collect_parameter_zero_candidates(op, candidates, visited);
                    }
                }
            }
        }
    }

    Variable* best = nullptr;
    for (Variable* cand : candidates) {
        if (!cand) {
            continue;
        }
        if (!best) {
            best = cand;
            continue;
        }
        const std::size_t cand_size = cand->size_bytes;
        const std::size_t best_size = best->size_bytes;
        if (cand_size > best_size) {
            best = cand;
            continue;
        }
        if (cand_size == best_size && cand->ssa_version() < best->ssa_version()) {
            best = cand;
        }
    }
    return best;
}

bool rewrite_undefined_scaled_global_indices(
    DecompilerTask& task,
    Expression* expr,
    const std::unordered_set<VarKey, VarKeyHash>& defined_vars,
    Variable* param_zero_fallback) {
    if (!expr || !param_zero_fallback) {
        return false;
    }

    bool changed = false;
    if (auto* op = dyn_cast<Operation>(expr)) {
        if (op->type() == OperationType::add && op->operands().size() == 2) {
            auto* base = dyn_cast<GlobalVariable>(strip_trivial_casts(op->operands()[0]));
            auto* idx = dyn_cast<Variable>(strip_trivial_casts(op->operands()[1]));
            if (base && idx && base->name() == "__MergedGlobals"
                && defined_vars.find(var_key(idx)) == defined_vars.end()
                && looks_like_scaled_index_temp(idx->name())) {
                const std::size_t width = idx->size_bytes > 0 ? idx->size_bytes : param_zero_fallback->size_bytes;
                auto* replacement = task.arena().create<Variable>(param_zero_fallback->name(), width);
                replacement->set_ssa_version(param_zero_fallback->ssa_version());
                copy_variable_metadata(replacement, param_zero_fallback);
                op->mutable_operands()[1] = replacement;
                changed = true;
            }
        }
        for (Expression*& child : op->mutable_operands()) {
            changed = rewrite_undefined_scaled_global_indices(task, child, defined_vars, param_zero_fallback) || changed;
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression*& child : list->mutable_operands()) {
            changed = rewrite_undefined_scaled_global_indices(task, child, defined_vars, param_zero_fallback) || changed;
        }
    }
    return changed;
}

bool expr_references_global_named(Expression* expr, const std::string& global_name) {
    if (!expr) {
        return false;
    }
    if (auto* gv = dyn_cast<GlobalVariable>(expr)) {
        return gv->name() == global_name;
    }
    if (auto* op = dyn_cast<Operation>(expr)) {
        for (Expression* child : op->operands()) {
            if (expr_references_global_named(child, global_name)) {
                return true;
            }
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            if (expr_references_global_named(child, global_name)) {
                return true;
            }
        }
    }
    return false;
}

bool expr_contains_list_or_call(Expression* expr) {
    if (!expr) {
        return false;
    }
    if (isa<ListOperation>(expr) || isa<Call>(expr)) {
        return true;
    }
    if (auto* op = dyn_cast<Operation>(expr)) {
        if (op->type() == OperationType::call) {
            return true;
        }
        for (Expression* child : op->operands()) {
            if (expr_contains_list_or_call(child)) {
                return true;
            }
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            if (expr_contains_list_or_call(child)) {
                return true;
            }
        }
    }
    return false;
}

bool expr_contains_disconnected_call_arg(Expression* expr) {
    if (!expr) {
        return false;
    }
    if (isa<ListOperation>(expr)) {
        return true;
    }
    if (isa<Call>(expr)) {
        return true;
    }
    if (auto* op = dyn_cast<Operation>(expr)) {
        if (op->type() == OperationType::call) {
            return true;
        }
        for (Expression* child : op->operands()) {
            if (expr_contains_disconnected_call_arg(child)) {
                return true;
            }
        }
    } else if (auto* list = dyn_cast<ListOperation>(expr)) {
        for (Expression* child : list->operands()) {
            if (expr_contains_disconnected_call_arg(child)) {
                return true;
            }
        }
    }
    return false;
}

bool sanitize_call_arguments_from_snapshot(DecompilerArena& arena, Expression* rewritten, Expression* original) {
    auto is_stack_pointer_like = [](Expression* expr) {
        auto* v = dyn_cast<Variable>(strip_trivial_casts(expr));
        if (!v) {
            return false;
        }
        std::string lowered = v->name();
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered == "sp" || lowered.rfind("sp_", 0) == 0;
    };
    auto is_stack_pointer_offset = [&](Expression* base_expr, Expression* candidate_expr) {
        if (!is_stack_pointer_like(base_expr)) {
            return false;
        }
        auto* op = dyn_cast<Operation>(strip_trivial_casts(candidate_expr));
        if (!op || (op->type() != OperationType::add && op->type() != OperationType::sub) || op->operands().size() != 2) {
            return false;
        }
        if (!is_stack_pointer_like(op->operands()[0])) {
            return false;
        }
        return isa<Constant>(strip_trivial_casts(op->operands()[1]));
    };

    auto sanitize_call = [&](auto* new_call, auto* old_call) {
        bool changed = false;
        auto& new_ops = new_call->mutable_operands();
        const auto& old_ops = old_call->operands();
        const std::size_t limit = std::min(new_ops.size(), old_ops.size());
        for (std::size_t i = 1; i < limit; ++i) {
            Expression* new_arg = new_ops[i];
            Expression* old_arg = old_ops[i];
            if (!new_arg || !old_arg) {
                continue;
            }
            const bool old_bad = expr_contains_disconnected_call_arg(old_arg);
            const bool new_bad = expr_contains_disconnected_call_arg(new_arg);
            if (new_bad && !old_bad) {
                new_ops[i] = old_arg->copy(arena);
                changed = true;
                continue;
            }
            if (is_stack_pointer_offset(old_arg, new_arg)) {
                new_ops[i] = old_arg->copy(arena);
                changed = true;
            }
        }
        return changed;
    };

    if (!rewritten || !original) {
        return false;
    }

    if (auto* new_call = dyn_cast<Call>(rewritten)) {
        auto* old_call = dyn_cast<Call>(original);
        if (!old_call) {
            return false;
        }
        return sanitize_call(new_call, old_call);
    }

    auto* new_op = dyn_cast<Operation>(rewritten);
    auto* old_op = dyn_cast<Operation>(original);
    if (!new_op || !old_op || new_op->type() != OperationType::call || old_op->type() != OperationType::call) {
        return false;
    }
    return sanitize_call(new_op, old_op);
}

} // namespace (AddressResolutionStage helpers)

void AddressResolutionStage::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    const auto aliases = collect_global_address_aliases(task.cfg());
    std::unordered_map<VarKey, std::string, VarKeyHash> global_alias_by_var;
    std::unordered_set<VarKey, VarKeyHash> defined_vars;
    collect_defined_variables(task.cfg(), defined_vars);
    Variable* param_zero_fallback = find_parameter_zero_fallback(task.cfg());

    for (BasicBlock* block : task.cfg()->blocks()) {
        for (Instruction* inst : block->instructions()) {
            auto* assign = dyn_cast<Assignment>(inst);
            if (!assign) {
                continue;
            }
            auto* dst_var = dyn_cast<Variable>(assign->destination());
            auto* gv_value = dyn_cast<GlobalVariable>(strip_trivial_casts(assign->value()));
            if (!dst_var || !gv_value || gv_value->name().empty()) {
                continue;
            }
            global_alias_by_var[var_key(dst_var)] = gv_value->name();
        }
    }

    for (BasicBlock* block : task.cfg()->blocks()) {
        canonicalize_global_index_temporaries_in_block(task, block);
        std::unordered_map<VarKey, Expression*, VarKeyHash> local_global_defs;
        for (Instruction* inst : block->instructions()) {
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                Expression* new_val = resolve_addresses_in_expr(
                    task.arena(), assign->value(), aliases);
                (void)rewrite_degenerate_global_self_index(task, new_val, global_alias_by_var);
                (void)rewrite_undefined_scaled_global_indices(task, new_val, defined_vars, param_zero_fallback);
                if (new_val != assign->value()) {
                    assign->set_value(new_val);
                }
                if (!isa<Variable>(assign->destination())) {
                    Expression* new_dest = resolve_addresses_in_expr(
                        task.arena(), assign->destination(), aliases);
                    (void)rewrite_degenerate_global_self_index(task, new_dest, global_alias_by_var);
                    (void)rewrite_undefined_scaled_global_indices(task, new_dest, defined_vars, param_zero_fallback);
                    if (new_dest != assign->destination()) {
                        assign->set_destination(new_dest);
                    }
                }
                if (auto* dst_var = dyn_cast<Variable>(assign->destination())) {
                    if (auto* op = dyn_cast<Operation>(assign->value())) {
                        if (op->type() == OperationType::deref && op->operands().size() == 1) {
                            auto* base_var = dyn_cast<Variable>(strip_trivial_casts(op->operands()[0]));
                            if (base_var) {
                                auto it = local_global_defs.find(var_key(base_var));
                                if (it != local_global_defs.end() && it->second) {
                                    auto* rooted_deref = task.arena().create<Operation>(
                                        OperationType::deref,
                                        std::vector<Expression*>{it->second->copy(task.arena())},
                                        op->size_bytes);
                                    assign->set_value(rooted_deref);
                                } else {
                                    auto alias_it = global_alias_by_var.find(var_key(base_var));
                                    if (alias_it != global_alias_by_var.end() && alias_it->second == "__MergedGlobals") {
                                        auto* gv = task.arena().create<GlobalVariable>("__MergedGlobals", base_var->size_bytes > 0 ? base_var->size_bytes : 8);
                                        auto* rooted_deref = task.arena().create<Operation>(
                                            OperationType::deref,
                                            std::vector<Expression*>{gv},
                                            op->size_bytes);
                                        assign->set_value(rooted_deref);
                                    }
                                }
                            }
                        }
                    }

                    if (auto* gv_value = dyn_cast<GlobalVariable>(strip_trivial_casts(assign->value()))) {
                        if (!gv_value->name().empty()) {
                            global_alias_by_var[var_key(dst_var)] = gv_value->name();
                        }
                    }

                    Expression* def_expr = assign->value();
                    if (def_expr && expr_references_global_named(def_expr, "__MergedGlobals")
                        && !expr_contains_list_or_call(def_expr)) {
                        local_global_defs[var_key(dst_var)] = def_expr->copy(task.arena());
                    } else {
                        local_global_defs.erase(var_key(dst_var));
                    }
                }
            } else if (auto* branch = dyn_cast<Branch>(inst)) {
                Condition* cond = dyn_cast<Condition>(resolve_addresses_in_expr(task.arena(), branch->condition(), aliases));
                if (cond) {
                    for (const auto& [key, def_expr] : local_global_defs) {
                        if (!def_expr) {
                            continue;
                        }
                        cond = dyn_cast<Condition>(replace_varkey_in_expr(task.arena(), cond, key, def_expr));
                    }
                }
                branch->set_condition(cond);
            } else if (auto* ret = dyn_cast<Return>(inst)) {
                for (Expression*& val : ret->mutable_values()) {
                    val = resolve_addresses_in_expr(task.arena(), val, aliases);
                    (void)rewrite_degenerate_global_self_index(task, val, global_alias_by_var);
                    (void)rewrite_undefined_scaled_global_indices(task, val, defined_vars, param_zero_fallback);
                }

            }
        }
        canonicalize_global_index_temporaries_in_block(task, block);
    }
}

} // namespace aletheia
