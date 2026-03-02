#include "operation_simplifier.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace logos {

namespace {

bool is_const_bool(DagNode* node, bool value) {
    auto* c = dag_dyn_cast<DagConstant>(node);
    if (!c) return false;
    return (c->value() != 0) == value;
}

bool is_not_of(DagNode* lhs, DagNode* rhs) {
    auto* op = dag_dyn_cast<DagOperation>(lhs);
    if (!op || op->op() != LogicOp::Not || op->children().size() != 1) return false;
    return op->children()[0]->to_string() == rhs->to_string();
}

DagNode* make_bool_constant(LogicDag& dag, bool value) {
    return dag.create_node<DagConstant>(value ? 1 : 0);
}

DagOperation* make_unary(LogicDag& dag, LogicOp op, DagNode* child) {
    auto* n = dag.create_node<DagOperation>(op);
    n->add_child(child);
    return n;
}

DagOperation* make_nary(LogicDag& dag, LogicOp op, const std::vector<DagNode*>& children) {
    auto* n = dag.create_node<DagOperation>(op);
    for (DagNode* child : children) {
        n->add_child(child);
    }
    return n;
}

std::vector<DagNode*> simplify_children(LogicDag& dag, DagOperation* op) {
    std::vector<DagNode*> out;
    out.reserve(op->children().size());
    for (DagNode* child : op->children()) {
        out.push_back(simplify_node(dag, child));
    }
    return out;
}

std::vector<DagNode*> flatten_associative(LogicOp op, const std::vector<DagNode*>& children) {
    std::vector<DagNode*> out;
    for (DagNode* child : children) {
        auto* child_op = dag_dyn_cast<DagOperation>(child);
        if (child_op && child_op->op() == op) {
            for (DagNode* grand : child_op->children()) {
                out.push_back(grand);
            }
        } else {
            out.push_back(child);
        }
    }
    return out;
}

DagNode* fold_all_constant_operation(LogicDag& dag, LogicOp op, const std::vector<DagNode*>& children) {
    std::vector<uint64_t> values;
    values.reserve(children.size());
    for (DagNode* child : children) {
        auto* c = dag_dyn_cast<DagConstant>(child);
        if (!c) return nullptr;
        values.push_back(c->value());
    }

    if (values.empty()) return nullptr;

    switch (op) {
        case LogicOp::And: {
            bool all_true = true;
            for (uint64_t v : values) {
                if (v == 0) return make_bool_constant(dag, false);
                all_true = all_true && (v != 0);
            }
            return make_bool_constant(dag, all_true);
        }
        case LogicOp::Or: {
            for (uint64_t v : values) {
                if (v != 0) return make_bool_constant(dag, true);
            }
            return make_bool_constant(dag, false);
        }
        case LogicOp::Not:
            if (values.size() == 1) return make_bool_constant(dag, values[0] == 0);
            return nullptr;
        case LogicOp::Eq:
            if (values.size() >= 2) return make_bool_constant(dag, values[0] == values[1]);
            return nullptr;
        case LogicOp::Neq:
            if (values.size() >= 2) return make_bool_constant(dag, values[0] != values[1]);
            return nullptr;
        case LogicOp::Lt:
            if (values.size() >= 2) return make_bool_constant(dag, values[0] < values[1]);
            return nullptr;
        case LogicOp::Le:
            if (values.size() >= 2) return make_bool_constant(dag, values[0] <= values[1]);
            return nullptr;
        case LogicOp::Gt:
            if (values.size() >= 2) return make_bool_constant(dag, values[0] > values[1]);
            return nullptr;
        case LogicOp::Ge:
            if (values.size() >= 2) return make_bool_constant(dag, values[0] >= values[1]);
            return nullptr;
    }

    return nullptr;
}

DagNode* simplify_not(LogicDag& dag, const std::vector<DagNode*>& children) {
    if (children.size() != 1) {
        return make_nary(dag, LogicOp::Not, children);
    }

    DagNode* child = children[0];
    if (auto* c = dag_dyn_cast<DagConstant>(child)) {
        return make_bool_constant(dag, c->value() == 0);
    }

    // De Morgan: !(a && b) -> !a || !b ; !(a || b) -> !a && !b
    if (auto* op = dag_dyn_cast<DagOperation>(child)) {
        if (op->op() == LogicOp::And || op->op() == LogicOp::Or) {
            const LogicOp flipped = op->op() == LogicOp::And ? LogicOp::Or : LogicOp::And;
            std::vector<DagNode*> negated;
            negated.reserve(op->children().size());
            for (DagNode* grand : op->children()) {
                negated.push_back(make_unary(dag, LogicOp::Not, grand));
            }
            return simplify_node(dag, make_nary(dag, flipped, negated));
        }
    }

    return make_unary(dag, LogicOp::Not, child);
}

DagNode* simplify_commutative(LogicDag& dag, LogicOp op, const std::vector<DagNode*>& children) {
    std::vector<DagNode*> flat = flatten_associative(op, children);

    std::vector<DagNode*> deduped;
    std::unordered_set<std::string> seen;
    for (DagNode* child : flat) {
        const std::string key = child->to_string();
        if (seen.insert(key).second) {
            deduped.push_back(child);
        }
    }

    // Identity/absorbing constants + collision detection.
    if (op == LogicOp::And) {
        for (DagNode* child : deduped) {
            if (is_const_bool(child, false)) {
                return make_bool_constant(dag, false);
            }
        }
        deduped.erase(std::remove_if(deduped.begin(), deduped.end(), [](DagNode* child) {
            return is_const_bool(child, true);
        }), deduped.end());

        for (DagNode* a : deduped) {
            for (DagNode* b : deduped) {
                if (a != b && (is_not_of(a, b) || is_not_of(b, a))) {
                    return make_bool_constant(dag, false);
                }
            }
        }
    } else if (op == LogicOp::Or) {
        for (DagNode* child : deduped) {
            if (is_const_bool(child, true)) {
                return make_bool_constant(dag, true);
            }
        }
        deduped.erase(std::remove_if(deduped.begin(), deduped.end(), [](DagNode* child) {
            return is_const_bool(child, false);
        }), deduped.end());

        for (DagNode* a : deduped) {
            for (DagNode* b : deduped) {
                if (a != b && (is_not_of(a, b) || is_not_of(b, a))) {
                    return make_bool_constant(dag, true);
                }
            }
        }
    }

    if (deduped.empty()) {
        return make_bool_constant(dag, op == LogicOp::And);
    }
    if (deduped.size() == 1) {
        return deduped.front();
    }

    return make_nary(dag, op, deduped);
}

} // namespace

DagNode* simplify_node(LogicDag& dag, DagNode* node) {
    auto* op = dag_dyn_cast<DagOperation>(node);
    if (!op) {
        return node;
    }

    std::vector<DagNode*> children = simplify_children(dag, op);

    if (DagNode* folded = fold_all_constant_operation(dag, op->op(), children)) {
        return folded;
    }

    switch (op->op()) {
        case LogicOp::Not:
            return simplify_not(dag, children);
        case LogicOp::And:
        case LogicOp::Or:
            return simplify_commutative(dag, op->op(), children);
        default:
            return make_nary(dag, op->op(), children);
    }
}

} // namespace logos
