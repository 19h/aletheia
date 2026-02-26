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

    auto ensure_bv = [&](z3::expr e, unsigned size) {
        if (e.is_bool()) {
            return z3::ite(e, ctx_.bv_val(1, size), ctx_.bv_val(0, size));
        }
        if (e.is_bv()) {
            unsigned curr_sz = e.get_sort().bv_size();
            if (curr_sz < size) return z3::zext(e, size - curr_sz);
            if (curr_sz > size) return e.extract(size - 1, 0);
        }
        return e;
    };

    if (o->type() == dewolf::OperationType::deref && ops.size() == 1) {
        std::string name = "deref_";
        if (auto* v = dynamic_cast<dewolf::Variable*>(ops[0])) name += v->name();
        return ctx_.bv_const(name.c_str(), (o->size_bytes > 0 ? o->size_bytes : 8) * 8);
    }
    
    if (o->type() == dewolf::OperationType::eq && ops.size() == 2) {
        return ensure_bv(convert(ops[0]), 64) == ensure_bv(convert(ops[1]), 64);
    }
    if (o->type() == dewolf::OperationType::neq && ops.size() == 2) {
        return ensure_bv(convert(ops[0]), 64) != ensure_bv(convert(ops[1]), 64);
    }
    if (o->type() == dewolf::OperationType::lt && ops.size() == 2) {
        return z3::slt(ensure_bv(convert(ops[0]), 64), ensure_bv(convert(ops[1]), 64));
    }
    if (o->type() == dewolf::OperationType::le && ops.size() == 2) {
        return z3::sle(ensure_bv(convert(ops[0]), 64), ensure_bv(convert(ops[1]), 64));
    }
    if (o->type() == dewolf::OperationType::gt && ops.size() == 2) {
        return z3::sgt(ensure_bv(convert(ops[0]), 64), ensure_bv(convert(ops[1]), 64));
    }
    if (o->type() == dewolf::OperationType::ge && ops.size() == 2) {
        return z3::sge(ensure_bv(convert(ops[0]), 64), ensure_bv(convert(ops[1]), 64));
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
