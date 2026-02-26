#include "z3_logic.hpp"
#include <iostream>

namespace dewolf_logic {

z3::expr Z3Converter::convert(dewolf::DataflowObject* obj) {
    if (!obj) {
        return ctx_.bool_val(false);
    }
    
    if (auto* c = dynamic_cast<dewolf::Constant*>(obj)) {
        return convert_constant(c);
    } else if (auto* v = dynamic_cast<dewolf::Variable*>(obj)) {
        return convert_variable(v);
    } else if (auto* o = dynamic_cast<dewolf::Operation*>(obj)) {
        return convert_operation(o);
    }
    
    return ctx_.bool_val(false);
}

z3::expr Z3Converter::convert_constant(dewolf::Constant* c) {
    return ctx_.bv_val(c->value(), c->size_bytes * 8);
}

z3::expr Z3Converter::convert_variable(dewolf::Variable* v) {
    return ctx_.bv_const(v->name().c_str(), v->size_bytes * 8);
}

z3::expr Z3Converter::convert_operation(dewolf::Operation* o) {
    if (o->operands().empty()) return ctx_.bool_val(false);

    auto& ops = o->operands();
    
    if (o->type() == dewolf::OperationType::eq && ops.size() == 2) {
        return convert(ops[0]) == convert(ops[1]);
    }
    if (o->type() == dewolf::OperationType::neq && ops.size() == 2) {
        return convert(ops[0]) != convert(ops[1]);
    }
    if (o->type() == dewolf::OperationType::lt && ops.size() == 2) {
        return z3::slt(convert(ops[0]), convert(ops[1]));
    }
    if (o->type() == dewolf::OperationType::le && ops.size() == 2) {
        return z3::sle(convert(ops[0]), convert(ops[1]));
    }
    if (o->type() == dewolf::OperationType::gt && ops.size() == 2) {
        return z3::sgt(convert(ops[0]), convert(ops[1]));
    }
    if (o->type() == dewolf::OperationType::ge && ops.size() == 2) {
        return z3::sge(convert(ops[0]), convert(ops[1]));
    }

    if (o->type() == dewolf::OperationType::logical_and && ops.size() == 2) {
        return convert(ops[0]) && convert(ops[1]);
    }
    if (o->type() == dewolf::OperationType::logical_or && ops.size() == 2) {
        return convert(ops[0]) || convert(ops[1]);
    }
    if (o->type() == dewolf::OperationType::logical_not && ops.size() == 1) {
        return !convert(ops[0]);
    }

    // Default fallback
    return ctx_.bool_val(false);
}

} // namespace dewolf_logic
