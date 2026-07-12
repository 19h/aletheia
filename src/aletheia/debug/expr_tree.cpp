#include "expr_tree.hpp"
#include "ir_serializer.hpp"
#include <sstream>
#include <stack>
#include <unordered_set>
#include <format>
#include <limits>

namespace aletheia::debug {

// ---- Self-reference detection (semantic: name + SSA version match) ----

bool has_self_reference(const Expression* src, const Variable* dst) {
    if (!src || !dst) return false;

    // Iterative DFS to avoid stack overflow
    std::stack<const Expression*> work;
    work.push(src);

    while (!work.empty()) {
        const Expression* node = work.top();
        work.pop();
        if (!node) continue;

        switch (node->node_kind()) {
            case NodeKind::Variable:
            case NodeKind::GlobalVariable: {
                auto* v = cast<Variable>(node);
                if (v->name() == dst->name() && v->ssa_version() == dst->ssa_version()) {
                    return true;
                }
                break;
            }

            case NodeKind::Operation:
            case NodeKind::Call:
            case NodeKind::Condition: {
                auto* op = cast<Operation>(node);
                for (auto* operand : op->operands()) {
                    work.push(operand);
                }
                break;
            }

            case NodeKind::ListOperation: {
                auto* lo = cast<ListOperation>(node);
                for (auto* operand : lo->operands()) {
                    work.push(operand);
                }
                break;
            }

            default:
                break; // Constant, etc. — leaf
        }
    }
    return false;
}

// ---- Expression metrics ----

namespace {

struct BoundedExpressionMetrics {
    std::size_t depth = 0;
    std::size_t weight = 0;
    bool invalid_or_exhausted = false;
};

constexpr std::size_t kMaxMetricExpansions = 1'000'000;

std::vector<const Expression*> metric_children(const Expression* expression) {
    std::vector<const Expression*> result;
    if (const auto* list = dyn_cast<ListOperation>(expression)) {
        result.reserve(list->operands().size());
        for (const Expression* child : list->operands()) {
            if (child) result.push_back(child);
        }
    } else if (const auto* operation = dyn_cast<Operation>(expression)) {
        result.reserve(operation->operands().size());
        for (const Expression* child : operation->operands()) {
            if (child) result.push_back(child);
        }
    }
    return result;
}

BoundedExpressionMetrics compute_expression_metrics(
    const Expression* root) {
    if (!root) return {};
    struct Frame {
        const Expression* expression = nullptr;
        std::vector<const Expression*> children;
        std::size_t next_child = 0;
    };
    std::unordered_set<const Expression*> active;
    std::vector<Frame> stack;
    active.insert(root);
    stack.push_back({root, metric_children(root), 0});
    BoundedExpressionMetrics metrics{.depth = 1, .weight = 1};
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next_child >= frame.children.size()) {
            active.erase(frame.expression);
            stack.pop_back();
            continue;
        }
        const Expression* child = frame.children[frame.next_child++];
        if (!child) continue;
        if (active.contains(child)
            || metrics.weight >= kMaxMetricExpansions) {
            metrics.invalid_or_exhausted = true;
            return metrics;
        }
        ++metrics.weight;
        active.insert(child);
        stack.push_back({child, metric_children(child), 0});
        metrics.depth = std::max(metrics.depth, stack.size());
    }
    return metrics;
}

} // namespace

std::size_t expression_depth(const Expression* expr) {
    const BoundedExpressionMetrics metrics = compute_expression_metrics(expr);
    return metrics.invalid_or_exhausted
        ? std::numeric_limits<std::size_t>::max() : metrics.depth;
}

std::size_t expression_weight(const Expression* expr) {
    const BoundedExpressionMetrics metrics = compute_expression_metrics(expr);
    return metrics.invalid_or_exhausted
        ? std::numeric_limits<std::size_t>::max() : metrics.weight;
}

// ---- Tree rendering helpers ----

static void render_tree(const Expression* expr,
                        const Variable* dest,
                        std::ostringstream& ss,
                        std::unordered_set<const Expression*>& visited,
                        int indent,
                        std::size_t depth_remaining) {
    std::string pad(indent, ' ');

    if (!expr) {
        ss << pad << "<null>\n";
        return;
    }

    if (depth_remaining == 0) {
        ss << pad << "... (depth limit reached)\n";
        return;
    }

    // DAG shared-node detection
    if (visited.contains(expr)) {
        ss << pad << std::format("<shared: {}>\n", static_cast<const void*>(expr));
        return;
    }
    visited.insert(expr);

    switch (expr->node_kind()) {
        case NodeKind::Constant: {
            auto* c = cast<Constant>(expr);
            ss << pad << std::format("Constant(0x{:x} [i{}])\n", c->value(), c->size_bytes * 8);
            break;
        }

        case NodeKind::GlobalVariable: {
            auto* gv = cast<GlobalVariable>(expr);
            ss << pad << std::format("GlobalVar:{} [i{}]", gv->name(), gv->size_bytes * 8);
            if (dest && gv->name() == dest->name() && gv->ssa_version() == dest->ssa_version()) {
                ss << "  <<< SELF-REFERENCE (matches dst var:" << dest->name()
                   << "_" << dest->ssa_version() << ")";
            }
            ss << "\n";
            break;
        }

        case NodeKind::Variable: {
            auto* v = cast<Variable>(expr);
            ss << pad << std::format("var:{}_{} [i{}]", v->name(), v->ssa_version(), v->size_bytes * 8);
            if (dest && v->name() == dest->name() && v->ssa_version() == dest->ssa_version()) {
                ss << "  <<< SELF-REFERENCE (matches dst var:" << dest->name()
                   << "_" << dest->ssa_version() << ")";
            }
            ss << "\n";
            break;
        }

        case NodeKind::Call: {
            auto* call = cast<Call>(expr);
            ss << pad << "Call:\n";
            ss << pad << "  target:\n";
            render_tree(call->target(), dest, ss, visited, indent + 4, depth_remaining - 1);
            for (std::size_t i = 0; i < call->arg_count(); ++i) {
                ss << pad << "  arg[" << i << "]:\n";
                render_tree(call->arg(i), dest, ss, visited, indent + 4, depth_remaining - 1);
            }
            break;
        }

        case NodeKind::Condition: {
            auto* cond = cast<Condition>(expr);
            ss << pad << "Condition(" << operation_type_name(cond->type()) << "):\n";
            ss << pad << "  lhs:\n";
            render_tree(cond->lhs(), dest, ss, visited, indent + 4, depth_remaining - 1);
            ss << pad << "  rhs:\n";
            render_tree(cond->rhs(), dest, ss, visited, indent + 4, depth_remaining - 1);
            break;
        }

        case NodeKind::Operation: {
            auto* op = cast<Operation>(expr);
            ss << pad << "Operation(" << operation_type_name(op->type()) << "):\n";
            for (std::size_t i = 0; i < op->operands().size(); ++i) {
                ss << pad << "  op[" << i << "]:\n";
                render_tree(op->operands()[i], dest, ss, visited, indent + 4, depth_remaining - 1);
            }
            break;
        }

        case NodeKind::ListOperation: {
            auto* lo = cast<ListOperation>(expr);
            ss << pad << "List:\n";
            for (std::size_t i = 0; i < lo->operands().size(); ++i) {
                ss << pad << "  [" << i << "]:\n";
                render_tree(lo->operands()[i], dest, ss, visited, indent + 4, depth_remaining - 1);
            }
            break;
        }

        default:
            ss << pad << std::format("<unknown_expr(kind={})>\n",
                                     static_cast<int>(expr->node_kind()));
            break;
    }
}

std::string expr_tree(const Expression* expr, std::size_t max_depth) {
    std::ostringstream ss;
    std::unordered_set<const Expression*> visited;
    render_tree(expr, nullptr, ss, visited, 0, max_depth);
    return ss.str();
}

std::string expr_tree_with_dest(const Expression* expr,
                                const Variable* assignment_dest,
                                std::size_t max_depth) {
    std::ostringstream ss;
    std::unordered_set<const Expression*> visited;
    render_tree(expr, assignment_dest, ss, visited, 0, max_depth);
    return ss.str();
}

} // namespace aletheia::debug
