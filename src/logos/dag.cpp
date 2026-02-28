#include "dag.hpp"

namespace logos {

std::string DagOperation::to_string() const {
    std::string op_str;
    switch (op_) {
        case LogicOp::And: op_str = "AND"; break;
        case LogicOp::Or: op_str = "OR"; break;
        case LogicOp::Not: op_str = "NOT"; break;
        case LogicOp::Eq: op_str = "=="; break;
        case LogicOp::Neq: op_str = "!="; break;
        case LogicOp::Lt: op_str = "<"; break;
        case LogicOp::Le: op_str = "<="; break;
        case LogicOp::Gt: op_str = ">"; break;
        case LogicOp::Ge: op_str = ">="; break;
    }

    std::string result = "(" + op_str;
    for (const auto& child : children()) {
        result += " " + child->to_string();
    }
    result += ")";
    return result;
}

} // namespace logos
