#include "range_simplifier.hpp"
#include "bitwise_simplifier.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <limits>

namespace logos {

// =============================================================================
// Helper utilities (adapted from Python ref's utils.py)
// =============================================================================

/// Return (min, max) for a given bit_size, signed or unsigned.
static std::pair<std::int64_t, std::int64_t> get_min_max_for_size(
    std::size_t bit_size, bool is_signed)
{
    if (bit_size == 0 || bit_size > 64) bit_size = 64;
    if (is_signed) {
        // signed: max = 2^(size-1) - 1, min = -max - 1
        auto max_val = static_cast<std::int64_t>((1ULL << (bit_size - 1)) - 1);
        auto min_val = -max_val - 1;
        return {min_val, max_val};
    } else {
        // unsigned: max = 2^size - 1, min = 0
        std::uint64_t max_val = (bit_size >= 64) ? std::numeric_limits<std::uint64_t>::max()
                                                  : ((1ULL << bit_size) - 1);
        return {0, static_cast<std::int64_t>(max_val)};
    }
}

/// Compare two values interpreting them as signed or unsigned.
static bool smaller(std::int64_t a, std::int64_t b, bool is_signed) {
    if (is_signed) return a < b;
    return static_cast<std::uint64_t>(a) < static_cast<std::uint64_t>(b);
}

/// Is the given OperationType a comparison?
static bool is_comparison(aletheia::OperationType op) {
    using OT = aletheia::OperationType;
    switch (op) {
        case OT::eq: case OT::neq:
        case OT::lt: case OT::le: case OT::gt: case OT::ge:
        case OT::lt_us: case OT::le_us: case OT::gt_us: case OT::ge_us:
            return true;
        default:
            return false;
    }
}

/// Is the given comparison a signed comparison?
static bool is_signed_comparison(aletheia::OperationType op) {
    using OT = aletheia::OperationType;
    switch (op) {
        case OT::eq: case OT::neq:
        case OT::lt: case OT::le: case OT::gt: case OT::ge:
            return true;
        default:
            return false;
    }
}

/// Is the comparison a strict inequality (< or >)?
static bool is_strict_comparison(aletheia::OperationType op) {
    using OT = aletheia::OperationType;
    return op == OT::lt || op == OT::gt || op == OT::lt_us || op == OT::gt_us;
}

/// Is the comparison a non-equal (<=, <, >, >=) relation?
static bool is_non_equal_relation(aletheia::OperationType op) {
    using OT = aletheia::OperationType;
    return op == OT::lt || op == OT::le || op == OT::gt || op == OT::ge
        || op == OT::lt_us || op == OT::le_us || op == OT::gt_us || op == OT::ge_us;
}

/// For a relation `A op B`, determine which operand position is the
/// "smaller" side and which is the "greater" side.
/// For `A < B` and `A <= B`: smaller = A (left), greater = B (right).
/// For `A > B` and `A >= B`: smaller = B (right), greater = A (left).
/// Returns: {smaller_is_lhs, greater_is_lhs}
/// If the op is eq or neq, returns {false, false} (no ordering).
struct OrderInfo {
    bool smaller_is_lhs;
    bool greater_is_lhs;
    bool has_ordering;
};

static OrderInfo get_order_info(aletheia::OperationType op) {
    using OT = aletheia::OperationType;
    switch (op) {
        case OT::lt: case OT::le: case OT::lt_us: case OT::le_us:
            // A < B or A <= B: A is smaller, B is greater
            return {true, false, true};
        case OT::gt: case OT::ge: case OT::gt_us: case OT::ge_us:
            // A > B or A >= B: B is smaller, A is greater
            return {false, true, true};
        default:
            return {false, false, false};
    }
}

// =============================================================================
// ExpressionValues
// =============================================================================

ExpressionValues::ExpressionValues(std::size_t bit_size)
    : bit_size_(bit_size)
{
    auto [smin, smax] = get_min_max_for_size(bit_size, true);
    auto [umin, umax] = get_min_max_for_size(bit_size, false);
    signed_min_ = smin;
    signed_max_ = smax;
    unsigned_min_ = static_cast<std::uint64_t>(umin);
    unsigned_max_ = static_cast<std::uint64_t>(umax);
}

void ExpressionValues::update_with(
    aletheia::OperationType op, std::int64_t const_val, bool const_is_lhs)
{
    using OT = aletheia::OperationType;

    // For eq/neq, the side doesn't matter
    if (op == OT::eq) {
        must_values_.insert(const_val);
        return;
    }
    if (op == OT::neq) {
        forbidden_values_.insert(const_val);
        return;
    }

    // For ordering comparisons, we need to figure out the bound.
    // The relation is: (const_val) op (variable) if const_is_lhs,
    //              or: (variable) op (const_val) if !const_is_lhs.
    //
    // We want to express bounds on the variable.
    // Examples:
    //   variable < const_val  => upper bound = const_val (strict)
    //   variable <= const_val => upper bound = const_val
    //   variable > const_val  => lower bound = const_val (strict)
    //   variable >= const_val => lower bound = const_val
    //   const_val < variable  => lower bound = const_val (strict)
    //   const_val <= variable => lower bound = const_val
    //   const_val > variable  => upper bound = const_val (strict)
    //   const_val >= variable => upper bound = const_val

    bool is_signed = is_signed_comparison(op);

    // Normalize: determine if const_val is an upper or lower bound for variable
    auto info = get_order_info(op);
    if (!info.has_ordering) return;

    // Determine if constant is on the "greater" side (upper bound for variable)
    // or "smaller" side (lower bound for variable).
    bool const_is_greater_side;
    if (const_is_lhs) {
        // relation is: const op variable
        // For lt/le: const is smaller, variable is greater => const is LOWER bound
        // For gt/ge: const is greater, variable is smaller => const is UPPER bound
        const_is_greater_side = info.greater_is_lhs;
    } else {
        // relation is: variable op const
        // For lt/le: variable is smaller, const is greater => const is UPPER bound
        // For gt/ge: variable is greater, const is smaller => const is LOWER bound
        const_is_greater_side = !info.greater_is_lhs;
    }

    // Adjust for strict inequality: x < c => x <= c-1, x > c => x >= c+1
    std::int64_t adjusted = const_val;
    if (is_strict_comparison(op)) {
        if (const_is_greater_side) {
            // const is upper bound, strict: upper = const - 1
            adjusted = const_val - 1;
        } else {
            // const is lower bound, strict: lower = const + 1
            adjusted = const_val + 1;
        }
    }

    if (const_is_greater_side) {
        // const is upper bound for variable
        upper_bound_.update_upper(adjusted, is_signed);
    } else {
        // const is lower bound for variable
        lower_bound_.update_lower(adjusted, is_signed);
    }
}

bool ExpressionValues::is_unfulfillable() const {
    // 1. More than one must-value
    if (must_values_.size() > 1) return true;

    // 2. Must-value is in forbidden set
    for (auto mv : must_values_) {
        if (forbidden_values_.count(mv)) return true;
    }

    // 3. Must-value is out of bounds
    if (must_values_.size() == 1) {
        auto mv = *must_values_.begin();
        // Check upper bound
        if (upper_bound_.signed_val.has_value() && mv > *upper_bound_.signed_val)
            return true;
        if (upper_bound_.unsigned_val.has_value() &&
            static_cast<std::uint64_t>(mv) > *upper_bound_.unsigned_val)
            return true;
        // Check lower bound
        if (lower_bound_.signed_val.has_value() && mv < *lower_bound_.signed_val)
            return true;
        if (lower_bound_.unsigned_val.has_value() &&
            static_cast<std::uint64_t>(mv) < *lower_bound_.unsigned_val)
            return true;
    }

    // 4. Upper < lower
    if (upper_less_than_lower()) return true;

    return false;
}

bool ExpressionValues::upper_less_than_lower() const {
    // Signed: upper.signed < lower.signed
    if (lower_bound_.signed_val.has_value() && upper_bound_.signed_val.has_value()) {
        if (*upper_bound_.signed_val < *lower_bound_.signed_val)
            return true;
    }
    // Unsigned: upper.unsigned < lower.unsigned
    if (lower_bound_.unsigned_val.has_value() && upper_bound_.unsigned_val.has_value()) {
        if (*upper_bound_.unsigned_val < *lower_bound_.unsigned_val)
            return true;
    }
    return false;
}

void ExpressionValues::simplify() {
    // 1. Remove redundant bounds (combine mixed signed/unsigned bounds)
    combine_mixed_bounds();

    // 2. Add must-value if bounds equal size bounds
    //    (e.g., lower >= INT_MAX means value must be INT_MAX)
    if (lower_bound_.signed_val.has_value() && *lower_bound_.signed_val == signed_max_)
        must_values_.insert(signed_max_);
    if (upper_bound_.signed_val.has_value() && *upper_bound_.signed_val == signed_min_)
        must_values_.insert(signed_min_);
    if (lower_bound_.unsigned_val.has_value() && *lower_bound_.unsigned_val == unsigned_max_)
        must_values_.insert(static_cast<std::int64_t>(unsigned_max_));
    if (upper_bound_.unsigned_val.has_value() && *upper_bound_.unsigned_val == unsigned_min_)
        must_values_.insert(static_cast<std::int64_t>(unsigned_min_));

    // 3. Refine bounds using forbidden values
    refine_bounds_using_forbidden();

    // 4. Remove redundant forbidden values (outside bounds)
    remove_redundant_forbidden();

    // 5. Add must-value if lower == upper
    add_must_if_bounds_equal();

    // 6. Recheck: add must-value if bounds equal size bounds (after refinement)
    if (lower_bound_.signed_val.has_value() && *lower_bound_.signed_val == signed_max_)
        must_values_.insert(signed_max_);
    if (upper_bound_.signed_val.has_value() && *upper_bound_.signed_val == signed_min_)
        must_values_.insert(signed_min_);
    if (lower_bound_.unsigned_val.has_value() && *lower_bound_.unsigned_val == unsigned_max_)
        must_values_.insert(static_cast<std::int64_t>(unsigned_max_));
    if (upper_bound_.unsigned_val.has_value() && *upper_bound_.unsigned_val == unsigned_min_)
        must_values_.insert(static_cast<std::int64_t>(unsigned_min_));
}

void ExpressionValues::refine_bounds_using_forbidden() {
    // While upper or lower bound is in the forbidden set, adjust it
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 1000) {
        changed = false;
        ++iterations;

        // Check upper bound signed
        if (upper_bound_.signed_val.has_value() &&
            forbidden_values_.count(*upper_bound_.signed_val))
        {
            upper_bound_.signed_val = *upper_bound_.signed_val - 1;
            changed = true;
            // Check if upper now equals lower -> must value
            if (lower_bound_.signed_val.has_value() &&
                *upper_bound_.signed_val == *lower_bound_.signed_val)
                must_values_.insert(*upper_bound_.signed_val);
        }

        // Check upper bound unsigned
        if (upper_bound_.unsigned_val.has_value() &&
            forbidden_values_.count(static_cast<std::int64_t>(*upper_bound_.unsigned_val)))
        {
            upper_bound_.unsigned_val = *upper_bound_.unsigned_val - 1;
            changed = true;
            if (lower_bound_.unsigned_val.has_value() &&
                *upper_bound_.unsigned_val == *lower_bound_.unsigned_val)
                must_values_.insert(static_cast<std::int64_t>(*upper_bound_.unsigned_val));
        }

        // Check lower bound signed
        if (lower_bound_.signed_val.has_value() &&
            forbidden_values_.count(*lower_bound_.signed_val))
        {
            lower_bound_.signed_val = *lower_bound_.signed_val + 1;
            changed = true;
            if (upper_bound_.signed_val.has_value() &&
                *lower_bound_.signed_val == *upper_bound_.signed_val)
                must_values_.insert(*lower_bound_.signed_val);
        }

        // Check lower bound unsigned
        if (lower_bound_.unsigned_val.has_value() &&
            forbidden_values_.count(static_cast<std::int64_t>(*lower_bound_.unsigned_val)))
        {
            lower_bound_.unsigned_val = *lower_bound_.unsigned_val + 1;
            changed = true;
            if (upper_bound_.unsigned_val.has_value() &&
                *lower_bound_.unsigned_val == *upper_bound_.unsigned_val)
                must_values_.insert(static_cast<std::int64_t>(*lower_bound_.unsigned_val));
        }
    }
}

void ExpressionValues::remove_redundant_forbidden() {
    // Remove forbidden values that are already outside the bounds
    std::erase_if(forbidden_values_, [&](std::int64_t val) {
        // If above upper bound (signed), it's already excluded
        if (upper_bound_.signed_val.has_value() && val > *upper_bound_.signed_val)
            return true;
        if (lower_bound_.signed_val.has_value() && val < *lower_bound_.signed_val)
            return true;
        if (upper_bound_.unsigned_val.has_value() &&
            static_cast<std::uint64_t>(val) > *upper_bound_.unsigned_val)
            return true;
        if (lower_bound_.unsigned_val.has_value() &&
            static_cast<std::uint64_t>(val) < *lower_bound_.unsigned_val)
            return true;
        return false;
    });
}

void ExpressionValues::add_must_if_bounds_equal() {
    // If signed lower == signed upper, add as must value
    if (lower_bound_.signed_val.has_value() && upper_bound_.signed_val.has_value() &&
        *lower_bound_.signed_val == *upper_bound_.signed_val)
    {
        must_values_.insert(*lower_bound_.signed_val);
    }
    // If unsigned lower == unsigned upper, add as must value
    if (lower_bound_.unsigned_val.has_value() && upper_bound_.unsigned_val.has_value() &&
        *lower_bound_.unsigned_val == *upper_bound_.unsigned_val)
    {
        must_values_.insert(static_cast<std::int64_t>(*lower_bound_.unsigned_val));
    }
}

void ExpressionValues::combine_mixed_bounds() {
    // Adapt the Python reference's 4-case bound combination logic.
    // This resolves situations where both signed and unsigned bounds exist
    // on the same side (upper or lower) or cross-side.

    auto signed_max_val = signed_max_;

    // Case 1: Both signed and unsigned upper bounds
    if (upper_bound_.signed_val.has_value() && upper_bound_.unsigned_val.has_value()) {
        auto s = *upper_bound_.signed_val;
        auto u = *upper_bound_.unsigned_val;
        if (u <= static_cast<std::uint64_t>(signed_max_val) && s < 0) {
            // x u<= c_u and x s<= c_s with c_u in signed range and c_s < 0
            // => implies x must be >= 0, so add signed lower bound of 0
            if (!lower_bound_.signed_val.has_value() || *lower_bound_.signed_val < 0)
                lower_bound_.signed_val = 0;
            upper_bound_.unsigned_val.reset();
        } else if (u <= static_cast<std::uint64_t>(signed_max_val) && s >= 0) {
            // Both in non-negative range: keep the tighter unsigned bound
            if (u > static_cast<std::uint64_t>(s))
                upper_bound_.unsigned_val = static_cast<std::uint64_t>(s);
            upper_bound_.signed_val.reset();
        } else if (u > static_cast<std::uint64_t>(signed_max_val) && s < 0) {
            // Unsigned extends beyond signed max, signed is negative
            auto u_as_signed = static_cast<std::int64_t>(u);
            if (u_as_signed <= s)
                upper_bound_.signed_val = u_as_signed;
            upper_bound_.unsigned_val.reset();
        }
        // else: both are "large" — keep both
    }

    // Case 2: Both signed and unsigned lower bounds
    if (lower_bound_.signed_val.has_value() && lower_bound_.unsigned_val.has_value()) {
        auto s = *lower_bound_.signed_val;
        auto u = *lower_bound_.unsigned_val;
        if (s >= 0 && u > static_cast<std::uint64_t>(signed_max_val)) {
            // x u>= c_u with c_u > signed_max and x s>= c_s with c_s >= 0
            // => x must be < 0 signed, so add signed upper = -1
            if (!upper_bound_.signed_val.has_value() || *upper_bound_.signed_val >= 0)
                upper_bound_.signed_val = -1;
            lower_bound_.unsigned_val.reset();
        } else if (u <= static_cast<std::uint64_t>(signed_max_val) && s >= 0) {
            // Both in non-negative range: keep the tighter signed bound
            if (s < static_cast<std::int64_t>(u))
                lower_bound_.signed_val = static_cast<std::int64_t>(u);
            lower_bound_.unsigned_val.reset();
        } else if (u > static_cast<std::uint64_t>(signed_max_val) && s < 0) {
            // Both in "negative" signed territory
            auto s_as_unsigned = static_cast<std::uint64_t>(s);
            if (s_as_unsigned >= u)
                lower_bound_.unsigned_val = s_as_unsigned;
            lower_bound_.signed_val.reset();
        }
    }

    // Case 3: Signed lower + unsigned upper
    if (lower_bound_.signed_val.has_value() && upper_bound_.unsigned_val.has_value()) {
        auto s = *lower_bound_.signed_val;
        auto u = *upper_bound_.unsigned_val;
        if (u <= static_cast<std::uint64_t>(signed_max_val) && s < 0) {
            // x u<= c_u and x s>= c_s with c_u in signed range, c_s negative
            // => just keep the unsigned upper (c_s is irrelevant, x can be anything u<=c_u)
            lower_bound_.signed_val.reset();
        } else if (u <= static_cast<std::uint64_t>(signed_max_val) && s >= 0) {
            // Both non-negative: convert signed lower to unsigned
            if (s > 0)
                lower_bound_.unsigned_val = static_cast<std::uint64_t>(s);
            lower_bound_.signed_val.reset();
        } else if (u > static_cast<std::uint64_t>(signed_max_val) && s >= 0) {
            // Unsigned upper extends beyond signed max
            upper_bound_.unsigned_val.reset();
        }
    }

    // Case 4: Signed upper + unsigned lower
    if (upper_bound_.signed_val.has_value() && lower_bound_.unsigned_val.has_value()) {
        auto s = *upper_bound_.signed_val;
        auto u = *lower_bound_.unsigned_val;
        if (u <= static_cast<std::uint64_t>(signed_max_val) && s < 0) {
            // x u>= c_u and x s<= c_s with c_u in signed range, c_s negative
            // => keep signed upper
            lower_bound_.unsigned_val.reset();
        } else if (u > static_cast<std::uint64_t>(signed_max_val) && s < 0) {
            // Both in "negative" territory: convert unsigned lower to signed
            auto u_as_signed = static_cast<std::int64_t>(u);
            if (u_as_signed != signed_min_)
                lower_bound_.signed_val = u_as_signed;
            lower_bound_.unsigned_val.reset();
        } else if (u > static_cast<std::uint64_t>(signed_max_val) && s >= 0) {
            // x u>= c_u and x s<= c_s with c_u > signed_max and c_s >= 0
            // => keep unsigned lower
            upper_bound_.signed_val.reset();
        }
    }
}

// =============================================================================
// BoundRelation
// =============================================================================

std::optional<BoundRelation> BoundRelation::from(aletheia::Expression* expr) {
    using OT = aletheia::OperationType;

    // Must be a Condition (binary comparison)
    auto* cond = aletheia::dyn_cast<aletheia::Condition>(expr);
    if (!cond) {
        // Also accept a generic Operation with a comparison type and 2 operands
        auto* op = aletheia::dyn_cast<aletheia::Operation>(expr);
        if (!op || op->operands().size() != 2 || !is_comparison(op->type()))
            return std::nullopt;
        // Use the Operation directly
        auto* lhs = op->operands()[0];
        auto* rhs = op->operands()[1];
        auto* lhs_const = aletheia::dyn_cast<aletheia::Constant>(lhs);
        auto* rhs_const = aletheia::dyn_cast<aletheia::Constant>(rhs);

        // Exactly one operand must be a constant
        if ((!lhs_const && !rhs_const) || (lhs_const && rhs_const))
            return std::nullopt;

        BoundRelation br;
        br.op = op->type();
        if (lhs_const) {
            br.variable_expr = rhs;
            br.constant_value = static_cast<std::int64_t>(lhs_const->value());
            br.constant_is_lhs = true;
        } else {
            br.variable_expr = lhs;
            br.constant_value = static_cast<std::int64_t>(rhs_const->value());
            br.constant_is_lhs = false;
        }
        return br;
    }

    auto* lhs = cond->lhs();
    auto* rhs = cond->rhs();
    auto* lhs_const = aletheia::dyn_cast<aletheia::Constant>(lhs);
    auto* rhs_const = aletheia::dyn_cast<aletheia::Constant>(rhs);

    // Exactly one operand must be a constant
    if ((!lhs_const && !rhs_const) || (lhs_const && rhs_const))
        return std::nullopt;

    if (!is_comparison(cond->type()))
        return std::nullopt;

    BoundRelation br;
    br.op = cond->type();
    if (lhs_const) {
        br.variable_expr = rhs;
        br.constant_value = static_cast<std::int64_t>(lhs_const->value());
        br.constant_is_lhs = true;
    } else {
        br.variable_expr = lhs;
        br.constant_value = static_cast<std::int64_t>(rhs_const->value());
        br.constant_is_lhs = false;
    }
    return br;
}

bool BoundRelation::is_signed() const {
    return is_signed_comparison(op);
}

// =============================================================================
// SingleRangeSimplifier
// =============================================================================

aletheia::Expression* SingleRangeSimplifier::simplify(
    aletheia::Expression* expr, aletheia::DecompilerArena& arena)
{
    using OT = aletheia::OperationType;

    // Must be a comparison operation with 2 operands
    auto* op = aletheia::dyn_cast<aletheia::Operation>(expr);
    if (!op || op->operands().size() != 2 || !is_comparison(op->type()))
        return nullptr;

    auto* lhs = op->operands()[0];
    auto* rhs = op->operands()[1];
    auto* lhs_const = aletheia::dyn_cast<aletheia::Constant>(lhs);
    auto* rhs_const = aletheia::dyn_cast<aletheia::Constant>(rhs);

    // Need at least one constant
    if (!lhs_const && !rhs_const) return nullptr;

    auto ot = op->type();
    bool is_signed = is_signed_comparison(ot);

    // Get size from the non-constant operand (or either constant)
    std::size_t bit_size = (lhs_const ? rhs : lhs)->size_bytes * 8;
    if (bit_size == 0) bit_size = 64;

    auto [min_val, max_val] = get_min_max_for_size(bit_size, is_signed);

    // For strict comparisons (< or >)
    if (is_strict_comparison(ot)) {
        auto info = get_order_info(ot);

        // Determine the smaller operand's bound and greater operand's bound
        aletheia::Expression* smaller_operand = info.smaller_is_lhs ? lhs : rhs;
        aletheia::Expression* greater_operand = info.smaller_is_lhs ? rhs : lhs;

        auto* smaller_const = aletheia::dyn_cast<aletheia::Constant>(smaller_operand);
        auto* greater_const = aletheia::dyn_cast<aletheia::Constant>(greater_operand);

        std::int64_t smaller_bound = smaller_const
            ? static_cast<std::int64_t>(smaller_const->value()) : min_val;
        std::int64_t greater_bound = greater_const
            ? static_cast<std::int64_t>(greater_const->value()) : max_val;

        // Check if unfulfillable: smaller_bound >= greater_bound
        bool unfulfillable;
        if (is_signed) {
            unfulfillable = smaller_bound >= greater_bound;
        } else {
            unfulfillable = static_cast<std::uint64_t>(smaller_bound) >=
                            static_cast<std::uint64_t>(greater_bound);
        }
        if (unfulfillable) {
            // Replace with false (0)
            return arena.create<aletheia::Constant>(0, 1);
        }

        // Check if consecutive: |smaller_bound - greater_bound| == 1
        // If so, replace with equality
        std::int64_t diff;
        if (is_signed) {
            diff = greater_bound - smaller_bound;
        } else {
            diff = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(greater_bound) -
                static_cast<std::uint64_t>(smaller_bound));
        }
        if (diff == 1) {
            // The only possible value for the variable is its size bound
            // Replace: var op const -> var == size_bound
            aletheia::Expression* var_operand = smaller_const ? greater_operand : smaller_operand;
            std::int64_t var_bound;
            if (smaller_const) {
                // Variable is on the greater side, and diff==1 means it equals greater_bound
                // Actually: var > const means var >= const+1, but if const+1 == max_val,
                // then var == max_val. In general, var must equal the only possible value.
                var_bound = smaller_const ? (greater_const ? greater_bound : max_val)
                                          : min_val;
            } else {
                var_bound = greater_const ? min_val : max_val;
            }
            // Determine the exact bound value for the variable
            if (!smaller_const) {
                // Variable is on the smaller side: var < greater_bound, and gap is 1
                // so var must equal greater_bound - 1 which is min_val (since smaller_bound == min_val
                // and greater_bound == min_val + 1... no, smaller_bound == min_val only if var is not const)
                // Simpler approach: the variable's size_bound is its extreme value
                // For smaller side: var's upper limit is greater_bound - 1
                var_bound = greater_bound - 1;
            } else {
                // Variable is on the greater side: var > smaller_bound, and gap is 1
                var_bound = smaller_bound + 1;
            }
            auto* bound_const = arena.create<aletheia::Constant>(
                static_cast<std::uint64_t>(var_bound), var_operand->size_bytes);
            return arena.create<aletheia::Condition>(OT::eq, var_operand, bound_const, 1);
        }
    }

    // For non-strict inequality (<= or >=)
    if (ot == OT::le || ot == OT::ge || ot == OT::le_us || ot == OT::ge_us) {
        auto info = get_order_info(ot);
        aletheia::Expression* smaller_operand = info.smaller_is_lhs ? lhs : rhs;
        aletheia::Expression* greater_operand = info.smaller_is_lhs ? rhs : lhs;

        auto* smaller_const = aletheia::dyn_cast<aletheia::Constant>(smaller_operand);
        auto* greater_const = aletheia::dyn_cast<aletheia::Constant>(greater_operand);

        std::int64_t smaller_bound = smaller_const
            ? static_cast<std::int64_t>(smaller_const->value()) : min_val;
        std::int64_t greater_bound = greater_const
            ? static_cast<std::int64_t>(greater_const->value()) : max_val;

        // Tautology check: if one operand is the size bound
        // e.g., x <= MAX_VAL is always true, MIN_VAL <= x is always true
        if (smaller_const && static_cast<std::int64_t>(smaller_const->value()) == min_val) {
            return arena.create<aletheia::Constant>(1, 1); // always true
        }
        if (greater_const && static_cast<std::int64_t>(greater_const->value()) == max_val) {
            return arena.create<aletheia::Constant>(1, 1); // always true
        }

        // Equality check: smaller_bound == greater_bound => replace with ==
        bool bounds_equal;
        if (is_signed) {
            bounds_equal = smaller_bound == greater_bound;
        } else {
            bounds_equal = static_cast<std::uint64_t>(smaller_bound) ==
                           static_cast<std::uint64_t>(greater_bound);
        }
        if (bounds_equal) {
            return arena.create<aletheia::Condition>(OT::eq, lhs, rhs, 1);
        }
    }

    return nullptr; // no simplification
}

// =============================================================================
// BitwiseAndRangeSimplifier
// =============================================================================

bool BitwiseAndRangeSimplifier::is_unfulfillable(
    aletheia::Expression* const* and_data, std::size_t and_count)
{
    // Wrap raw pointer+count for range-based iteration
    struct OperandRange {
        aletheia::Expression* const* d; std::size_t n;
        aletheia::Expression* const* begin() const { return d; }
        aletheia::Expression* const* end() const { return d + n; }
        std::size_t size() const { return n; }
    };
    OperandRange and_operands{and_data, and_count};
    // Build ExpressionValues for each distinct variable expression
    // We use pointer identity for grouping (same Variable* = same variable)
    struct VarInfo {
        aletheia::Expression* var_expr;
        ExpressionValues values;
    };

    // Map variable pointer -> ExpressionValues
    std::vector<VarInfo> var_infos;

    auto find_or_create = [&](aletheia::Expression* var_expr, std::size_t bit_size) -> ExpressionValues& {
        for (auto& vi : var_infos) {
            // Match by pointer identity or by Variable name+ssa_version
            if (vi.var_expr == var_expr) return vi.values;
            auto* v1 = aletheia::dyn_cast<aletheia::Variable>(vi.var_expr);
            auto* v2 = aletheia::dyn_cast<aletheia::Variable>(var_expr);
            if (v1 && v2 && v1->name() == v2->name() &&
                v1->ssa_version() == v2->ssa_version())
                return vi.values;
        }
        var_infos.push_back({var_expr, ExpressionValues(bit_size)});
        return var_infos.back().values;
    };

    for (auto* operand : and_operands) {
        auto br = BoundRelation::from(operand);
        if (!br) continue;

        std::size_t bit_size = br->variable_expr->size_bytes * 8;
        if (bit_size == 0) bit_size = 64;

        auto& ev = find_or_create(br->variable_expr, bit_size);
        ev.update_with(br->op, br->constant_value, br->constant_is_lhs);
    }

    // Check each variable's constraints for unfulfillability
    for (auto& vi : var_infos) {
        vi.values.simplify();
        if (vi.values.is_unfulfillable())
            return true;
    }

    return false;
}

aletheia::Expression* BitwiseAndRangeSimplifier::simplify(
    aletheia::Operation* op, aletheia::DecompilerArena& arena)
{
    if (!op || (op->type() != aletheia::OperationType::logical_and && op->type() != aletheia::OperationType::bit_and)) {
        return op;
    }

    auto& and_operands = op->mutable_operands();

    // 1. Simplify individual operands
    bool any_changed = false;
    for (auto& child : and_operands) {
        auto* simplified = SingleRangeSimplifier::simplify(child, arena);
        if (simplified && simplified != child) {
            child = simplified;
            any_changed = true;
        }
    }

    // 2. Remove operands that simplified to true (Constant 1)
    auto initial_size = and_operands.size();
    and_operands.erase_if([](aletheia::Expression* e) {
        auto* c = aletheia::dyn_cast<aletheia::Constant>(e);
        return c && c->value() == 1;
    });
    if (and_operands.size() != initial_size) any_changed = true;

    // 3. If any operand is false, the whole AND is false
    for (auto* child : and_operands) {
        auto* c = aletheia::dyn_cast<aletheia::Constant>(child);
        if (c && c->value() == 0) {
            return arena.create<aletheia::Constant>(0, 1);
        }
    }

    // 4. Check for unfulfillability using the full constraints
    if (is_unfulfillable(and_operands.data(), and_operands.size())) {
        return arena.create<aletheia::Constant>(0, 1);
    }

    // 5. Emit replacement constraints (e.g. combined bounds, must-values)
    // Build ExpressionValues for each distinct variable expression
    struct VarInfo {
        aletheia::Expression* var_expr;
        ExpressionValues values;
    };
    std::vector<VarInfo> var_infos;

    auto find_or_create = [&](aletheia::Expression* var_expr, std::size_t bit_size) -> ExpressionValues& {
        for (auto& vi : var_infos) {
            if (vi.var_expr == var_expr) return vi.values;
            auto* v1 = aletheia::dyn_cast<aletheia::Variable>(vi.var_expr);
            auto* v2 = aletheia::dyn_cast<aletheia::Variable>(var_expr);
            if (v1 && v2 && v1->name() == v2->name() &&
                v1->ssa_version() == v2->ssa_version())
                return vi.values;
        }
        var_infos.push_back({var_expr, ExpressionValues(bit_size)});
        return var_infos.back().values;
    };

    // Filter out operands that can be converted to BoundRelation and process them
    std::vector<aletheia::Expression*> remaining_operands;
    for (auto* operand : and_operands) {
        auto br = BoundRelation::from(operand);
        if (br) {
            std::size_t bit_size = br->variable_expr->size_bytes * 8;
            if (bit_size == 0) bit_size = 64;
            auto& ev = find_or_create(br->variable_expr, bit_size);
            ev.update_with(br->op, br->constant_value, br->constant_is_lhs);
        } else {
            remaining_operands.push_back(operand);
        }
    }

    // Generate new operands based on simplified ExpressionValues
    std::vector<aletheia::Expression*> new_operands;
    for (auto& vi : var_infos) {
        vi.values.simplify();
        auto* var_expr = vi.var_expr;

        if (vi.values.is_unfulfillable()) {
            return arena.create<aletheia::Constant>(0, 1);
        }

        if (vi.values.must_values().size() == 1) {
            auto val = *vi.values.must_values().begin();
            auto* const_expr = arena.create<aletheia::Constant>(static_cast<std::uint64_t>(val), var_expr->size_bytes);
            new_operands.push_back(arena.create<aletheia::Condition>(aletheia::OperationType::eq, var_expr, const_expr, 1));
        } else {
            // Emit bounds constraints
            const auto& upper = vi.values.upper_bound();
            if (upper.signed_val) {
                auto* const_expr = arena.create<aletheia::Constant>(static_cast<std::uint64_t>(*upper.signed_val), var_expr->size_bytes);
                new_operands.push_back(arena.create<aletheia::Condition>(aletheia::OperationType::le, var_expr, const_expr, 1));
            }
            if (upper.unsigned_val) {
                auto* const_expr = arena.create<aletheia::Constant>(*upper.unsigned_val, var_expr->size_bytes);
                new_operands.push_back(arena.create<aletheia::Condition>(aletheia::OperationType::le_us, var_expr, const_expr, 1));
            }

            const auto& lower = vi.values.lower_bound();
            if (lower.signed_val) {
                auto* const_expr = arena.create<aletheia::Constant>(static_cast<std::uint64_t>(*lower.signed_val), var_expr->size_bytes);
                new_operands.push_back(arena.create<aletheia::Condition>(aletheia::OperationType::ge, var_expr, const_expr, 1));
            }
            if (lower.unsigned_val) {
                auto* const_expr = arena.create<aletheia::Constant>(*lower.unsigned_val, var_expr->size_bytes);
                new_operands.push_back(arena.create<aletheia::Condition>(aletheia::OperationType::ge_us, var_expr, const_expr, 1));
            }

            // Emit inequality constraints for consecutive ranges
            auto fvals = vi.values.forbidden_values();
            if (!fvals.empty()) {
                std::vector<std::int64_t> sorted_fvals(fvals.begin(), fvals.end());
                std::sort(sorted_fvals.begin(), sorted_fvals.end(), [](std::int64_t a, std::int64_t b) {
                    return static_cast<std::uint64_t>(a) < static_cast<std::uint64_t>(b);
                });

                std::vector<std::int64_t> current_row = {sorted_fvals[0]};
                for (size_t i = 1; i < sorted_fvals.size(); ++i) {
                    if (static_cast<std::uint64_t>(current_row.back()) + 1 == static_cast<std::uint64_t>(sorted_fvals[i])) {
                        current_row.push_back(sorted_fvals[i]);
                    } else {
                        // Process current row
                        if (current_row.size() <= 2) {
                            for (auto c : current_row) {
                                auto* const_expr = arena.create<aletheia::Constant>(static_cast<std::uint64_t>(c), var_expr->size_bytes);
                                new_operands.push_back(arena.create<aletheia::Condition>(aletheia::OperationType::neq, var_expr, const_expr, 1));
                            }
                        } else {
                            // range exclusion using bitwise NOT of AND
                            auto* c1 = arena.create<aletheia::Constant>(static_cast<std::uint64_t>(current_row.front()), var_expr->size_bytes);
                            auto* lower_bound = arena.create<aletheia::Condition>(aletheia::OperationType::le_us, c1, var_expr, 1);
                            auto* c2 = arena.create<aletheia::Constant>(static_cast<std::uint64_t>(current_row.back()), var_expr->size_bytes);
                            auto* upper_bound = arena.create<aletheia::Condition>(aletheia::OperationType::le_us, var_expr, c2, 1);
                            auto* and_op = arena.create<aletheia::Operation>(aletheia::OperationType::bit_and, std::vector<aletheia::Expression*>{lower_bound, upper_bound}, 1);
                            new_operands.push_back(arena.create<aletheia::Operation>(aletheia::OperationType::logical_not, std::vector<aletheia::Expression*>{and_op}, 1));
                        }
                        current_row = {sorted_fvals[i]};
                    }
                }
                // Process last row
                if (current_row.size() <= 2) {
                    for (auto c : current_row) {
                        auto* const_expr = arena.create<aletheia::Constant>(static_cast<std::uint64_t>(c), var_expr->size_bytes);
                        new_operands.push_back(arena.create<aletheia::Condition>(aletheia::OperationType::neq, var_expr, const_expr, 1));
                    }
                } else {
                    auto* c1 = arena.create<aletheia::Constant>(static_cast<std::uint64_t>(current_row.front()), var_expr->size_bytes);
                    auto* lower_bound = arena.create<aletheia::Condition>(aletheia::OperationType::le_us, c1, var_expr, 1);
                    auto* c2 = arena.create<aletheia::Constant>(static_cast<std::uint64_t>(current_row.back()), var_expr->size_bytes);
                    auto* upper_bound = arena.create<aletheia::Condition>(aletheia::OperationType::le_us, var_expr, c2, 1);
                    auto* and_op = arena.create<aletheia::Operation>(aletheia::OperationType::bit_and, std::vector<aletheia::Expression*>{lower_bound, upper_bound}, 1);
                    new_operands.push_back(arena.create<aletheia::Operation>(aletheia::OperationType::logical_not, std::vector<aletheia::Expression*>{and_op}, 1));
                }
            }
        }
    }

    // Combine remaining and new operands
    new_operands.insert(new_operands.end(), remaining_operands.begin(), remaining_operands.end());

    if (new_operands.size() == 0) {
        return arena.create<aletheia::Constant>(1, 1);
    } else if (new_operands.size() == 1) {
        return new_operands[0];
    } else {
        // Did we actually change anything structurally?
        bool structure_changed = any_changed || new_operands.size() != and_operands.size();
        if (!structure_changed) {
            // Quick check if pointers are exactly the same
            for (size_t i = 0; i < new_operands.size(); ++i) {
                if (new_operands[i] != and_operands[i]) {
                    structure_changed = true;
                    break;
                }
            }
        }
        
        if (structure_changed) {
            return arena.create<aletheia::Operation>(op->type(), std::move(new_operands), 1);
        }
        return op;
    }
}

// =============================================================================
// BitwiseOrRangeSimplifier
// =============================================================================

aletheia::Expression* BitwiseOrRangeSimplifier::simplify(
    aletheia::Operation* op, aletheia::DecompilerArena& arena)
{
    if (!op || (op->type() != aletheia::OperationType::logical_or && op->type() != aletheia::OperationType::bit_or)) {
        return op;
    }

    // Simplify Or of ranges by negating the formula to an And condition and applying the BitwiseAndRangeSimplifier.
    // c1 | c2 | ... | cn = ~( ~c1 & ~c2 & ... & ~cn)

    std::vector<aletheia::Expression*> negated_operands;
    for (auto* child : op->operands()) {
        auto* negated_child = arena.create<aletheia::Operation>(aletheia::OperationType::logical_not, std::vector<aletheia::Expression*>{child}, 1);
        negated_operands.push_back(negated_child);
    }

    auto* new_and_op = arena.create<aletheia::Operation>(aletheia::OperationType::logical_and, std::move(negated_operands), 1);

    auto* simplified_and = BitwiseAndRangeSimplifier::simplify(new_and_op, arena);

    if (auto* c = aletheia::dyn_cast<aletheia::Constant>(simplified_and)) {
        if (c->value() == 0) {
            // Not(False) == True
            return arena.create<aletheia::Constant>(1, 1);
        } else if (c->value() == 1) {
            // Not(True) == False
            return arena.create<aletheia::Constant>(0, 1);
        }
    }

    // Negate it back
    return arena.create<aletheia::Operation>(aletheia::OperationType::logical_not, std::vector<aletheia::Expression*>{simplified_and}, 1);
}

// =============================================================================
// RangeSimplifier
// =============================================================================

bool RangeSimplifier::is_unfulfillable(aletheia::Expression* condition) {
    using OT = aletheia::OperationType;

    if (!condition) return false;

    // If it's a single comparison, try to simplify it
    auto br = BoundRelation::from(condition);
    if (br) {
        // Single relation: check if it's trivially false
        std::size_t bit_size = br->variable_expr->size_bytes * 8;
        if (bit_size == 0) bit_size = 64;
        ExpressionValues ev(bit_size);
        ev.update_with(br->op, br->constant_value, br->constant_is_lhs);
        ev.simplify();
        return ev.is_unfulfillable();
    }

    // If it's a logical AND, check all operands together
    auto* op = aletheia::dyn_cast<aletheia::Operation>(condition);
    if (op && op->type() == OT::logical_and) {
        // Collect all AND operands (potentially nested)
        std::vector<aletheia::Expression*> all_operands;
        std::function<void(aletheia::Expression*)> collect = [&](aletheia::Expression* e) {
            auto* inner = aletheia::dyn_cast<aletheia::Operation>(e);
            if (inner && inner->type() == OT::logical_and) {
                for (auto* child : inner->operands())
                    collect(child);
            } else {
                all_operands.push_back(e);
            }
        };
        collect(condition);
        return BitwiseAndRangeSimplifier::is_unfulfillable(all_operands.data(), all_operands.size());
    }

    // If it's a bit_and (bitwise AND used for logic), same treatment
    if (op && op->type() == OT::bit_and) {
        std::vector<aletheia::Expression*> all_operands;
        std::function<void(aletheia::Expression*)> collect = [&](aletheia::Expression* e) {
            auto* inner = aletheia::dyn_cast<aletheia::Operation>(e);
            if (inner && inner->type() == OT::bit_and) {
                for (auto* child : inner->operands())
                    collect(child);
            } else {
                all_operands.push_back(e);
            }
        };
        collect(condition);
        return BitwiseAndRangeSimplifier::is_unfulfillable(all_operands.data(), all_operands.size());
    }

    return false;
}

aletheia::Expression* RangeSimplifier::simplify(
    aletheia::Expression* condition, aletheia::DecompilerArena& arena)
{
    using OT = aletheia::OperationType;

    if (!condition) return condition;

    // Step 1: Try single range simplification on individual relations
    auto* single_result = SingleRangeSimplifier::simplify(condition, arena);
    if (single_result) return single_result;

    // Step 2: If it's a logical/bitwise AND, simplify each operand first,
    // then check the conjunction.
    auto* op = aletheia::dyn_cast<aletheia::Operation>(condition);
    if (op && (op->type() == OT::logical_and || op->type() == OT::bit_and)) {
        // Simplify each operand individually
        bool any_changed = false;
        auto& operands = op->mutable_operands();
        for (auto& child : operands) {
            auto* simplified = SingleRangeSimplifier::simplify(child, arena);
            if (simplified) {
                child = simplified;
                any_changed = true;
            }
        }

        // Remove operands that simplified to true (Constant 1)
        operands.erase_if([](aletheia::Expression* e) {
            auto* c = aletheia::dyn_cast<aletheia::Constant>(e);
            return c && c->value() == 1;
        });

        // If any operand is false, the whole AND is false
        for (auto* child : operands) {
            auto* c = aletheia::dyn_cast<aletheia::Constant>(child);
            if (c && c->value() == 0) {
                return arena.create<aletheia::Constant>(0, 1);
            }
        }

        // If only one operand left, return it directly
        if (operands.size() == 1) return operands[0];
        if (operands.empty()) return arena.create<aletheia::Constant>(1, 1);

        // Check conjunction unfulfillability
        if (BitwiseAndRangeSimplifier::is_unfulfillable(operands.data(), operands.size())) {
            return arena.create<aletheia::Constant>(0, 1);
        }

        return any_changed ? condition : condition;
    }

    // Step 3: If it's a logical/bitwise OR, simplify each operand
    if (op && (op->type() == OT::logical_or || op->type() == OT::bit_or)) {
        bool any_changed = false;
        auto& operands = op->mutable_operands();
        for (auto& child : operands) {
            auto* simplified = SingleRangeSimplifier::simplify(child, arena);
            if (simplified) {
                child = simplified;
                any_changed = true;
            }
        }

        // Remove operands that simplified to false (Constant 0)
        operands.erase_if([](aletheia::Expression* e) {
            auto* c = aletheia::dyn_cast<aletheia::Constant>(e);
            return c && c->value() == 0;
        });

        // If any operand is true, the whole OR is true
        for (auto* child : operands) {
            auto* c = aletheia::dyn_cast<aletheia::Constant>(child);
            if (c && c->value() == 1) {
                return arena.create<aletheia::Constant>(1, 1);
            }
        }

        if (operands.size() == 1) return operands[0];
        if (operands.empty()) return arena.create<aletheia::Constant>(0, 1);
    }

    return condition;
}

bool RangeSimplifier::is_unfulfillable(DagNode* condition) {
    DagNode* simplified = simplify(condition);
    auto* c = dag_dyn_cast<DagConstant>(simplified);
    return c != nullptr && c->value() == 0;
}

DagNode* RangeSimplifier::simplify(DagNode* condition) {
    if (!condition) {
        return nullptr;
    }

    auto* op = dag_dyn_cast<DagOperation>(condition);
    if (!op) {
        return condition;
    }

    if (op->op() == LogicOp::And) {
        DagBitwiseAndRangeSimplifier and_simplifier(&legacy_dag_);
        return and_simplifier.simplify(condition);
    }

    bool changed = false;
    std::vector<DagNode*> simplified_children;
    simplified_children.reserve(op->children().size());
    for (DagNode* child : op->children()) {
        DagNode* simplified_child = simplify(child);
        if (simplified_child != child) {
            changed = true;
        }
        simplified_children.push_back(simplified_child);
    }

    if (!changed) {
        return condition;
    }

    auto* rebuilt = legacy_dag_.create_node<DagOperation>(op->op());
    for (DagNode* child : simplified_children) {
        rebuilt->add_child(child);
    }
    return rebuilt;
}

} // namespace logos
