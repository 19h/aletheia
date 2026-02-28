#pragma once
#include "dag.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace logos {

enum class DagBoundRelation {
    Overlap,
    Disjoint,
    Subset,
    Superset,
    Equal
};

class DagExpressionValues {
public:
    DagExpressionValues() = default;

    void add_equal(std::int64_t value);
    void add_not_equal(std::int64_t value);
    void add_lower_bound(std::int64_t value);
    void add_upper_bound(std::int64_t value);

    void normalize();
    bool is_unfulfillable() const;

    bool has_equality() const { return equal_.has_value(); }
    std::int64_t equality() const { return equal_.value_or(0); }

    bool has_lower_bound() const { return has_lower_; }
    bool has_upper_bound() const { return has_upper_; }
    std::int64_t lower_bound() const { return lower_; }
    std::int64_t upper_bound() const { return upper_; }

    const std::unordered_set<std::int64_t>& forbidden_values() const { return not_equal_; }

private:
    bool contradiction_ = false;
    bool has_lower_ = false;
    bool has_upper_ = false;
    std::int64_t lower_ = std::numeric_limits<std::int64_t>::min();
    std::int64_t upper_ = std::numeric_limits<std::int64_t>::max();
    std::optional<std::int64_t> equal_;
    std::unordered_set<std::int64_t> not_equal_;
};

class DagBitwiseAndRangeSimplifier {
public:
    explicit DagBitwiseAndRangeSimplifier(LogicDag* dag = nullptr) : dag_(dag) {}

    /// Simplify conjunctions of comparison relations over constants.
    ///
    /// Supported relation forms:
    /// - `expr == const`, `expr != const`
    /// - `expr < const`, `expr <= const`, `expr > const`, `expr >= const`
    /// - and their constant-on-left variants (`const < expr`, ...)
    ///
    /// The method does NOT perform algebraic simplification outside this scope.
    /// It only normalizes/combines range constraints within a bitwise-And tree.
    DagNode* simplify(DagNode* condition);

private:
    template <typename T, typename... Args>
    T* make_node(Args&&... args) {
        if (dag_) {
            return dag_->create_node<T>(std::forward<Args>(args)...);
        }
        auto node = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = node.get();
        owned_nodes_.push_back(std::move(node));
        return ptr;
    }

    DagOperation* make_relation(LogicOp op, DagNode* lhs, DagNode* rhs);

    LogicDag* dag_ = nullptr;
    std::vector<std::unique_ptr<DagNode>> owned_nodes_;
};

} // namespace logos
