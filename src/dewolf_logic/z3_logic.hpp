#pragma once
#include <z3++.h>
#include <memory>
#include <cstdint>
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

    bool is_equivalent_to(const LogicCondition& other) const {
        // A <-> B means solver.check(!(A <-> B)) == unsat
        z3::solver solver(expr_.ctx());
        solver.add(!z3::implies(expr_, other.expression()) || !z3::implies(other.expression(), expr_));
        return solver.check() == z3::unsat;
    }

    bool is_complementary_to(const LogicCondition& other) const {
        // A == !B -> A <-> !B -> unsat(!(A <-> !B))
        z3::solver solver(expr_.ctx());
        solver.add(!z3::implies(expr_, !other.expression()) || !z3::implies(!other.expression(), expr_));
        return solver.check() == z3::unsat;
    }

    bool does_imply(const LogicCondition& other) const {
        // A -> B means solver.check(!(A -> B)) == unsat
        z3::solver solver(expr_.ctx());
        solver.add(!z3::implies(expr_, other.expression()));
        return solver.check() == z3::unsat;
    }

    bool is_satisfiable() const {
        z3::solver solver(expr_.ctx());
        solver.add(expr_);
        return solver.check() == z3::sat;
    }

    bool is_not_satisfiable() const {
        return !is_satisfiable();
    }

    LogicCondition negate() const {
        return LogicCondition(!expr_);
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
    LogicCondition convert_to_condition(dewolf::DataflowObject* obj);

private:
    z3::context& ctx_;
    std::uint64_t fresh_symbol_id_ = 0;

    z3::expr convert_constant(dewolf::Constant* c);
    z3::expr convert_variable(dewolf::Variable* v);
    z3::expr convert_operation(dewolf::Operation* o);

    z3::expr ensure_bool(z3::expr e);
    z3::expr ensure_bv(z3::expr e, unsigned size_bits);
    z3::expr fresh_bool(const char* prefix = "sym_bool");
    z3::expr fresh_bv(unsigned size_bits, const char* prefix = "sym_bv");
};

} // namespace dewolf_logic
