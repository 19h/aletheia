#pragma once
#include "dag.hpp"
#include "../dewolf/structures/dataflow.hpp"
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dewolf_logic {

// =============================================================================
// ConstantBound -- Tracks a signed and/or unsigned bound value
// =============================================================================
// A single variable can be simultaneously constrained by both signed and
// unsigned comparisons. ConstantBound tracks both independently. The
// simplification pipeline combines them when possible.

struct ConstantBound {
    std::optional<std::int64_t>  signed_val;
    std::optional<std::uint64_t> unsigned_val;

    bool has_any() const { return signed_val.has_value() || unsigned_val.has_value(); }

    /// Update upper bound: keep the tighter (smaller) value.
    void update_upper(std::int64_t val, bool is_signed) {
        if (is_signed) {
            if (!signed_val || val < *signed_val) signed_val = val;
        } else {
            auto uval = static_cast<std::uint64_t>(val);
            if (!unsigned_val || uval < *unsigned_val) unsigned_val = uval;
        }
    }

    /// Update lower bound: keep the tighter (larger) value.
    void update_lower(std::int64_t val, bool is_signed) {
        if (is_signed) {
            if (!signed_val || val > *signed_val) signed_val = val;
        } else {
            auto uval = static_cast<std::uint64_t>(val);
            if (!unsigned_val || uval > *unsigned_val) unsigned_val = uval;
        }
    }
};

// =============================================================================
// ExpressionValues -- Cumulative constraints on one variable
// =============================================================================
// Tracks must-values (==), forbidden-values (!=), and lower/upper bounds
// from <, <=, >, >= constraints. Detects unfulfillability and simplifies
// redundant constraints.

class ExpressionValues {
public:
    ExpressionValues(std::size_t bit_size = 64);

    /// Add a constraint from a bound relation.
    /// @param op The comparison type (eq, neq, lt, le, gt, ge, *_us variants)
    /// @param const_val The constant value in the comparison
    /// @param const_is_lhs True if the constant is the LHS of the comparison
    void update_with(dewolf::OperationType op, std::int64_t const_val, bool const_is_lhs);

    /// Check if the constraints are contradictory.
    bool is_unfulfillable() const;

    /// Simplify the constraint set (remove redundant bounds, detect must-values).
    void simplify();

    // --- Access ---
    const std::unordered_set<std::int64_t>& must_values() const { return must_values_; }
    const std::unordered_set<std::int64_t>& forbidden_values() const { return forbidden_values_; }
    const ConstantBound& lower_bound() const { return lower_bound_; }
    const ConstantBound& upper_bound() const { return upper_bound_; }

private:
    std::size_t bit_size_;
    std::int64_t  signed_min_, signed_max_;
    std::uint64_t unsigned_min_, unsigned_max_;

    std::unordered_set<std::int64_t> must_values_;
    std::unordered_set<std::int64_t> forbidden_values_;
    ConstantBound lower_bound_;
    ConstantBound upper_bound_;

    bool upper_less_than_lower() const;
    void refine_bounds_using_forbidden();
    void remove_redundant_forbidden();
    void add_must_if_bounds_equal();
    void combine_mixed_bounds();
};

// =============================================================================
// BoundRelation -- Validates and wraps a binary comparison with one constant
// =============================================================================
// Given an Expression* (typically a Condition or Operation), checks if it's
// a binary relation with exactly one Constant operand. If so, provides
// accessor methods for the variable, constant, and comparison type.

struct BoundRelation {
    dewolf::OperationType op;
    dewolf::Expression* variable_expr;  // The non-constant operand
    std::int64_t constant_value;
    bool constant_is_lhs;  // true if the constant is on the left side

    /// Try to create a BoundRelation from an expression.
    /// Returns nullopt if the expression is not a valid binary relation with
    /// exactly one constant.
    static std::optional<BoundRelation> from(dewolf::Expression* expr);

    /// Is this a signed comparison?
    bool is_signed() const;
};

// =============================================================================
// SingleRangeSimplifier -- Simplifies one binary relation in isolation
// =============================================================================

class SingleRangeSimplifier {
public:
    /// Attempt to simplify a single relation.
    /// Returns the simplified expression, or nullptr if no simplification was possible.
    /// The returned expression may be the original, or a Constant(0) for false,
    /// or a Constant(1) for true.
    static dewolf::Expression* simplify(dewolf::Expression* expr, dewolf::DecompilerArena& arena);
};

// =============================================================================
// BitwiseAndRangeSimplifier -- Simplifies a conjunction of range constraints
// =============================================================================

class BitwiseAndRangeSimplifier {
public:
    /// Attempt to simplify an AND of relations.
    /// Returns true if the conjunction is unfulfillable (the AND should become false).
    /// @param and_operands The operands of the AND operation.
    static bool is_unfulfillable(const std::vector<dewolf::Expression*>& and_operands);

    /// Attempt to simplify an AND operation.
    /// Returns a new Expression* (or Constant 0 if unfulfillable)
    static dewolf::Expression* simplify(dewolf::Operation* op, dewolf::DecompilerArena& arena);
};

// =============================================================================
// BitwiseOrRangeSimplifier -- Simplifies a disjunction of range constraints
// =============================================================================

class BitwiseOrRangeSimplifier {
public:
    /// Attempt to simplify an OR operation by negating, applying AND simplifier, and negating back.
    static dewolf::Expression* simplify(dewolf::Operation* op, dewolf::DecompilerArena& arena);
};

// =============================================================================
// RangeSimplifier -- Top-level orchestrator
// =============================================================================
// The public API for range simplification. Works on Expression* trees.

class RangeSimplifier {
public:
    RangeSimplifier() = default;

    /// Check if a condition expression is unfulfillable (always false).
    /// Handles AND (logical_and) conditions by extracting bound relations
    /// and checking for contradictions.
    bool is_unfulfillable(dewolf::Expression* condition);

    /// Simplify a condition expression, returning the simplified version.
    /// May return the original if no simplification was possible.
    dewolf::Expression* simplify(dewolf::Expression* condition, dewolf::DecompilerArena& arena);

    /// Legacy DAG interface (delegates to expression-based implementation).
    bool is_unfulfillable(DagNode* condition);

    DagNode* simplify(DagNode* condition);

private:
    // Storage for any DAG nodes created through the legacy API.
    LogicDag legacy_dag_;
};

} // namespace dewolf_logic
