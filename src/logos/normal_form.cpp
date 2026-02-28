#include "normal_form.hpp"

#include "operation_simplifier.hpp"

#include <vector>

namespace logos {

namespace {

DagNode* make_constant(LogicDag& dag, bool value) {
    return dag.create_node<DagConstant>(value ? 1 : 0);
}

DagNode* make_unary_not(LogicDag& dag, DagNode* child) {
    auto* op = dag.create_node<DagOperation>(LogicOp::Not);
    op->add_child(child);
    return op;
}

DagNode* make_nary(LogicDag& dag, LogicOp op, const std::vector<DagNode*>& children) {
    if (children.empty()) {
        if (op == LogicOp::And) return make_constant(dag, true);
        if (op == LogicOp::Or) return make_constant(dag, false);
    }
    if (children.size() == 1) {
        return children.front();
    }
    auto* node = dag.create_node<DagOperation>(op);
    for (DagNode* child : children) {
        node->add_child(child);
    }
    return node;
}

DagNode* to_nnf(LogicDag& dag, DagNode* node, bool negated) {
    if (auto* c = dynamic_cast<DagConstant*>(node)) {
        const bool value = c->value() != 0;
        return make_constant(dag, negated ? !value : value);
    }

    auto* op = dynamic_cast<DagOperation*>(node);
    if (!op) {
        return negated ? make_unary_not(dag, node) : node;
    }

    if (op->op() == LogicOp::Not && op->children().size() == 1) {
        return to_nnf(dag, op->children()[0], !negated);
    }

    if (op->op() == LogicOp::And || op->op() == LogicOp::Or) {
        LogicOp next = op->op();
        if (negated) {
            next = (next == LogicOp::And) ? LogicOp::Or : LogicOp::And;
        }

        std::vector<DagNode*> children;
        children.reserve(op->children().size());
        for (DagNode* child : op->children()) {
            children.push_back(to_nnf(dag, child, negated));
        }
        return make_nary(dag, next, children);
    }

    return negated ? make_unary_not(dag, node) : node;
}

std::vector<DagNode*> flatten_same_op(LogicOp op, DagNode* node) {
    std::vector<DagNode*> out;
    auto* n = dynamic_cast<DagOperation*>(node);
    if (n && n->op() == op) {
        for (DagNode* child : n->children()) {
            out.push_back(child);
        }
        return out;
    }
    out.push_back(node);
    return out;
}

DagNode* distribute_or_over_and_pair(LogicDag& dag, DagNode* lhs, DagNode* rhs) {
    auto* lhs_op = dynamic_cast<DagOperation*>(lhs);
    if (lhs_op && lhs_op->op() == LogicOp::And) {
        std::vector<DagNode*> expanded;
        expanded.reserve(lhs_op->children().size());
        for (DagNode* child : lhs_op->children()) {
            expanded.push_back(distribute_or_over_and_pair(dag, child, rhs));
        }
        return make_nary(dag, LogicOp::And, expanded);
    }

    auto* rhs_op = dynamic_cast<DagOperation*>(rhs);
    if (rhs_op && rhs_op->op() == LogicOp::And) {
        std::vector<DagNode*> expanded;
        expanded.reserve(rhs_op->children().size());
        for (DagNode* child : rhs_op->children()) {
            expanded.push_back(distribute_or_over_and_pair(dag, lhs, child));
        }
        return make_nary(dag, LogicOp::And, expanded);
    }

    std::vector<DagNode*> children;
    auto lhs_flat = flatten_same_op(LogicOp::Or, lhs);
    auto rhs_flat = flatten_same_op(LogicOp::Or, rhs);
    children.reserve(lhs_flat.size() + rhs_flat.size());
    children.insert(children.end(), lhs_flat.begin(), lhs_flat.end());
    children.insert(children.end(), rhs_flat.begin(), rhs_flat.end());
    return make_nary(dag, LogicOp::Or, children);
}

DagNode* distribute_and_over_or_pair(LogicDag& dag, DagNode* lhs, DagNode* rhs) {
    auto* lhs_op = dynamic_cast<DagOperation*>(lhs);
    if (lhs_op && lhs_op->op() == LogicOp::Or) {
        std::vector<DagNode*> expanded;
        expanded.reserve(lhs_op->children().size());
        for (DagNode* child : lhs_op->children()) {
            expanded.push_back(distribute_and_over_or_pair(dag, child, rhs));
        }
        return make_nary(dag, LogicOp::Or, expanded);
    }

    auto* rhs_op = dynamic_cast<DagOperation*>(rhs);
    if (rhs_op && rhs_op->op() == LogicOp::Or) {
        std::vector<DagNode*> expanded;
        expanded.reserve(rhs_op->children().size());
        for (DagNode* child : rhs_op->children()) {
            expanded.push_back(distribute_and_over_or_pair(dag, lhs, child));
        }
        return make_nary(dag, LogicOp::Or, expanded);
    }

    std::vector<DagNode*> children;
    auto lhs_flat = flatten_same_op(LogicOp::And, lhs);
    auto rhs_flat = flatten_same_op(LogicOp::And, rhs);
    children.reserve(lhs_flat.size() + rhs_flat.size());
    children.insert(children.end(), lhs_flat.begin(), lhs_flat.end());
    children.insert(children.end(), rhs_flat.begin(), rhs_flat.end());
    return make_nary(dag, LogicOp::And, children);
}

DagNode* to_cnf_impl(LogicDag& dag, DagNode* node) {
    auto* op = dynamic_cast<DagOperation*>(node);
    if (!op) {
        return node;
    }

    if (op->op() == LogicOp::And) {
        std::vector<DagNode*> children;
        children.reserve(op->children().size());
        for (DagNode* child : op->children()) {
            children.push_back(to_cnf_impl(dag, child));
        }
        return make_nary(dag, LogicOp::And, children);
    }

    if (op->op() == LogicOp::Or) {
        if (op->children().empty()) {
            return make_constant(dag, false);
        }
        DagNode* acc = to_cnf_impl(dag, op->children().front());
        for (std::size_t i = 1; i < op->children().size(); ++i) {
            DagNode* rhs = to_cnf_impl(dag, op->children()[i]);
            acc = distribute_or_over_and_pair(dag, acc, rhs);
        }
        return acc;
    }

    return node;
}

DagNode* to_dnf_impl(LogicDag& dag, DagNode* node) {
    auto* op = dynamic_cast<DagOperation*>(node);
    if (!op) {
        return node;
    }

    if (op->op() == LogicOp::Or) {
        std::vector<DagNode*> children;
        children.reserve(op->children().size());
        for (DagNode* child : op->children()) {
            children.push_back(to_dnf_impl(dag, child));
        }
        return make_nary(dag, LogicOp::Or, children);
    }

    if (op->op() == LogicOp::And) {
        if (op->children().empty()) {
            return make_constant(dag, true);
        }
        DagNode* acc = to_dnf_impl(dag, op->children().front());
        for (std::size_t i = 1; i < op->children().size(); ++i) {
            DagNode* rhs = to_dnf_impl(dag, op->children()[i]);
            acc = distribute_and_over_or_pair(dag, acc, rhs);
        }
        return acc;
    }

    return node;
}

} // namespace

DagNode* ToCnfVisitor::convert(LogicDag& dag, DagNode* root) {
    if (!root) {
        return nullptr;
    }
    DagNode* nnf = to_nnf(dag, root, false);
    return simplify_node(dag, to_cnf_impl(dag, nnf));
}

DagNode* ToDnfVisitor::convert(LogicDag& dag, DagNode* root) {
    if (!root) {
        return nullptr;
    }
    DagNode* nnf = to_nnf(dag, root, false);
    return simplify_node(dag, to_dnf_impl(dag, nnf));
}

} // namespace logos
