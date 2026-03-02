#include "bitwise_simplifier.hpp"

#include <algorithm>
#include <functional>

namespace logos {

namespace {

struct ParsedRelation {
    LogicOp op{};
    DagNode* expression = nullptr;
    std::string key;
    std::int64_t constant = 0;
    bool expression_on_lhs = true;
};

struct ConstraintBucket {
    DagNode* expression = nullptr;
    DagExpressionValues values;
};

std::int64_t saturating_increment(std::int64_t value) {
    if (value == std::numeric_limits<std::int64_t>::max()) {
        return value;
    }
    return value + 1;
}

std::int64_t saturating_decrement(std::int64_t value) {
    if (value == std::numeric_limits<std::int64_t>::min()) {
        return value;
    }
    return value - 1;
}

bool is_relation_op(LogicOp op) {
    switch (op) {
        case LogicOp::Eq:
        case LogicOp::Neq:
        case LogicOp::Lt:
        case LogicOp::Le:
        case LogicOp::Gt:
        case LogicOp::Ge:
            return true;
        default:
            return false;
    }
}

bool try_parse_relation(DagNode* node, ParsedRelation& out) {
    auto* rel = dag_dyn_cast<DagOperation>(node);
    if (!rel || !is_relation_op(rel->op())) {
        return false;
    }
    if (rel->children().size() != 2) {
        return false;
    }

    DagNode* lhs = rel->children()[0];
    DagNode* rhs = rel->children()[1];
    auto* lhs_const = dag_dyn_cast<DagConstant>(lhs);
    auto* rhs_const = dag_dyn_cast<DagConstant>(rhs);

    if ((lhs_const == nullptr) == (rhs_const == nullptr)) {
        // Either both constant or none constant: not a range relation we handle.
        return false;
    }

    out.op = rel->op();
    if (rhs_const != nullptr) {
        out.expression = lhs;
        out.key = lhs->to_string();
        out.constant = static_cast<std::int64_t>(rhs_const->value());
        out.expression_on_lhs = true;
    } else {
        out.expression = rhs;
        out.key = rhs->to_string();
        out.constant = static_cast<std::int64_t>(lhs_const->value());
        out.expression_on_lhs = false;
    }
    return true;
}

void collect_and_terms(DagNode* node, std::vector<DagNode*>& out) {
    auto* op = dag_dyn_cast<DagOperation>(node);
    if (op && op->op() == LogicOp::And) {
        for (DagNode* child : op->children()) {
            collect_and_terms(child, out);
        }
        return;
    }
    out.push_back(node);
}

} // namespace

void DagExpressionValues::add_equal(std::int64_t value) {
    if (equal_.has_value() && *equal_ != value) {
        contradiction_ = true;
        return;
    }
    equal_ = value;
}

void DagExpressionValues::add_not_equal(std::int64_t value) {
    not_equal_.insert(value);
}

void DagExpressionValues::add_lower_bound(std::int64_t value) {
    if (!has_lower_ || value > lower_) {
        lower_ = value;
        has_lower_ = true;
    }
}

void DagExpressionValues::add_upper_bound(std::int64_t value) {
    if (!has_upper_ || value < upper_) {
        upper_ = value;
        has_upper_ = true;
    }
}

void DagExpressionValues::normalize() {
    if (contradiction_) {
        return;
    }

    if (has_lower_ && has_upper_ && lower_ > upper_) {
        contradiction_ = true;
        return;
    }

    if (equal_.has_value()) {
        const std::int64_t eq = *equal_;
        if ((has_lower_ && eq < lower_) || (has_upper_ && eq > upper_)) {
            contradiction_ = true;
            return;
        }
        if (not_equal_.contains(eq)) {
            contradiction_ = true;
            return;
        }
        return;
    }

    // Remove forbidden values that are already outside known bounds.
    std::erase_if(not_equal_, [&](std::int64_t value) {
        if (has_lower_ && value < lower_) {
            return true;
        }
        if (has_upper_ && value > upper_) {
            return true;
        }
        return false;
    });

    if (has_lower_ && has_upper_ && lower_ == upper_) {
        equal_ = lower_;
        if (not_equal_.contains(*equal_)) {
            contradiction_ = true;
        }
    }
}

bool DagExpressionValues::is_unfulfillable() const {
    return contradiction_;
}

DagOperation* DagBitwiseAndRangeSimplifier::make_relation(LogicOp op, DagNode* lhs, DagNode* rhs) {
    auto* rel = make_node<DagOperation>(op);
    rel->add_child(lhs);
    rel->add_child(rhs);
    return rel;
}

DagNode* DagBitwiseAndRangeSimplifier::simplify(DagNode* condition) {
    if (condition == nullptr) {
        return nullptr;
    }

    auto* root = dag_dyn_cast<DagOperation>(condition);
    if (!root || root->op() != LogicOp::And) {
        return condition;
    }

    std::vector<DagNode*> terms;
    collect_and_terms(condition, terms);

    std::vector<DagNode*> passthrough;
    std::unordered_map<std::string, ConstraintBucket> constraints;
    bool changed = false;

    for (DagNode* term : terms) {
        if (auto* c = dag_dyn_cast<DagConstant>(term)) {
            changed = true;
            if (c->value() == 0) {
                return make_node<DagConstant>(0);
            }
            // Constant true in conjunction: drop.
            continue;
        }

        ParsedRelation rel;
        if (!try_parse_relation(term, rel)) {
            passthrough.push_back(term);
            continue;
        }

        changed = true;
        auto& bucket = constraints[rel.key];
        if (bucket.expression == nullptr) {
            bucket.expression = rel.expression;
        }

        switch (rel.op) {
            case LogicOp::Eq:
                bucket.values.add_equal(rel.constant);
                break;
            case LogicOp::Neq:
                bucket.values.add_not_equal(rel.constant);
                break;
            case LogicOp::Lt:
                if (rel.expression_on_lhs) {
                    bucket.values.add_upper_bound(saturating_decrement(rel.constant));
                } else {
                    bucket.values.add_lower_bound(saturating_increment(rel.constant));
                }
                break;
            case LogicOp::Le:
                if (rel.expression_on_lhs) {
                    bucket.values.add_upper_bound(rel.constant);
                } else {
                    bucket.values.add_lower_bound(rel.constant);
                }
                break;
            case LogicOp::Gt:
                if (rel.expression_on_lhs) {
                    bucket.values.add_lower_bound(saturating_increment(rel.constant));
                } else {
                    bucket.values.add_upper_bound(saturating_decrement(rel.constant));
                }
                break;
            case LogicOp::Ge:
                if (rel.expression_on_lhs) {
                    bucket.values.add_lower_bound(rel.constant);
                } else {
                    bucket.values.add_upper_bound(rel.constant);
                }
                break;
            default:
                break;
        }
    }

    std::vector<std::pair<std::string, ConstraintBucket*>> ordered;
    ordered.reserve(constraints.size());
    for (auto& [key, bucket] : constraints) {
        ordered.emplace_back(key, &bucket);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });

    std::vector<DagNode*> rebuilt;
    rebuilt.reserve(passthrough.size() + ordered.size() * 4);
    rebuilt.insert(rebuilt.end(), passthrough.begin(), passthrough.end());

    for (auto& [_, bucket_ptr] : ordered) {
        auto& bucket = *bucket_ptr;
        bucket.values.normalize();
        if (bucket.values.is_unfulfillable()) {
            return make_node<DagConstant>(0);
        }

        DagNode* expr = bucket.expression;
        if (expr == nullptr) {
            continue;
        }

        if (bucket.values.has_equality()) {
            rebuilt.push_back(make_relation(
                LogicOp::Eq,
                expr,
                make_node<DagConstant>(static_cast<std::uint64_t>(bucket.values.equality()))));
            continue;
        }

        if (bucket.values.has_lower_bound()) {
            rebuilt.push_back(make_relation(
                LogicOp::Ge,
                expr,
                make_node<DagConstant>(static_cast<std::uint64_t>(bucket.values.lower_bound()))));
        }
        if (bucket.values.has_upper_bound()) {
            rebuilt.push_back(make_relation(
                LogicOp::Le,
                expr,
                make_node<DagConstant>(static_cast<std::uint64_t>(bucket.values.upper_bound()))));
        }

        if (!bucket.values.forbidden_values().empty()) {
            std::vector<std::int64_t> forbidden(bucket.values.forbidden_values().begin(),
                                                bucket.values.forbidden_values().end());
            std::sort(forbidden.begin(), forbidden.end());
            for (std::int64_t value : forbidden) {
                rebuilt.push_back(make_relation(
                    LogicOp::Neq,
                    expr,
                    make_node<DagConstant>(static_cast<std::uint64_t>(value))));
            }
        }
    }

    if (!changed) {
        return condition;
    }

    if (rebuilt.empty()) {
        return make_node<DagConstant>(1);
    }
    if (rebuilt.size() == 1) {
        return rebuilt.front();
    }

    auto* and_node = make_node<DagOperation>(LogicOp::And);
    for (DagNode* child : rebuilt) {
        and_node->add_child(child);
    }
    return and_node;
}

} // namespace logos
