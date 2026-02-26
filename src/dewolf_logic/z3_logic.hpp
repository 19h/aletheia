#pragma once
#include <z3++.h>
#include <memory>
#include "../dewolf/structures/dataflow.hpp"
#include "../common/arena.hpp"

namespace dewolf_logic {

class LogicCondition {
public:
    LogicCondition(z3::expr expr) : expr_(expr) {}

    z3::expr expression() const { return expr_; }
    bool is_true() const { return expr_.is_true(); }
    bool is_false() const { return expr_.is_false(); }

    // Simplify the boolean condition via Z3
    LogicCondition simplify() const {
        z3::tactic t(expr_.ctx(), "simplify");
        z3::goal g(expr_.ctx());
        g.add(expr_);
        z3::apply_result res = t(g);
        if (res.size() > 0 && res[0].size() > 0) {
            return LogicCondition(res[0][0]);
        }
        return *this;
    }

private:
    z3::expr expr_;
};

class Z3Converter {
public:
    Z3Converter(z3::context& ctx) : ctx_(ctx) {}

    // Convert our internal AST DataflowObject to a Z3 expression
    z3::expr convert(dewolf::DataflowObject* obj);
    
    // Explicit condition conversions
    LogicCondition convert_to_condition(dewolf::DataflowObject* obj) {
        return LogicCondition(convert(obj));
    }

private:
    z3::context& ctx_;

    z3::expr convert_constant(dewolf::Constant* c);
    z3::expr convert_variable(dewolf::Variable* v);
    z3::expr convert_operation(dewolf::Operation* o);
};

} // namespace dewolf_logic
