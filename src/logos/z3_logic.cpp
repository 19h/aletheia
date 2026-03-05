#include "z3_logic.hpp"
#include <limits>
#include <string>

namespace logos {

namespace {

unsigned normalized_bit_width(std::size_t size_bytes) {
    constexpr unsigned kDefaultBits = 64;
    if (size_bytes == 0) {
        return kDefaultBits;
    }
    const std::size_t bits = size_bytes * 8;
    if (bits == 0) {
        return kDefaultBits;
    }
    if (bits > static_cast<std::size_t>(std::numeric_limits<unsigned>::max())) {
        return kDefaultBits;
    }
    return static_cast<unsigned>(bits);
}

bool is_boolean_operation(aletheia::OperationType type) {
    using aletheia::OperationType;
    switch (type) {
        case OperationType::eq:
        case OperationType::neq:
        case OperationType::lt:
        case OperationType::le:
        case OperationType::gt:
        case OperationType::ge:
        case OperationType::lt_us:
        case OperationType::le_us:
        case OperationType::gt_us:
        case OperationType::ge_us:
        case OperationType::logical_and:
        case OperationType::logical_or:
        case OperationType::logical_not:
            return true;
        default:
            return false;
    }
}

} // namespace

static std::string expr_fingerprint(aletheia::Expression* expr) {
    if (!expr) return "<null>";
    if (auto* c = aletheia::dyn_cast<aletheia::Constant>(expr)) {
        return "C:" + std::to_string(c->value()) + ":" + std::to_string(c->size_bytes);
    }
    if (auto* v = aletheia::dyn_cast<aletheia::Variable>(expr)) {
        return "V:" + v->name() + ":" + std::to_string(v->ssa_version());
    }
    if (auto* op = aletheia::dyn_cast<aletheia::Operation>(expr)) {
        std::string out = "O:" + std::to_string(static_cast<int>(op->type())) + "(";
        bool first = true;
        for (auto* child : op->operands()) {
            if (!first) out += ",";
            first = false;
            out += expr_fingerprint(child);
        }
        out += ")";
        return out;
    }
    return "E:unknown";
}

z3::expr Z3Converter::fresh_bool(const char* prefix) {
    const std::string name = std::string(prefix) + "_" + std::to_string(fresh_symbol_id_++);
    return ctx_.bool_const(name.c_str());
}

z3::expr Z3Converter::fresh_bv(unsigned size_bits, const char* prefix) {
    const unsigned width = size_bits == 0 ? 64U : size_bits;
    const std::string name = std::string(prefix) + "_" + std::to_string(fresh_symbol_id_++);
    return ctx_.bv_const(name.c_str(), width);
}

z3::expr Z3Converter::ensure_bool(z3::expr e) {
    if (e.is_bool()) {
        return e;
    }
    if (e.is_bv()) {
        return e != ctx_.bv_val(0, e.get_sort().bv_size());
    }
    return fresh_bool("coerce_bool");
}

z3::expr Z3Converter::ensure_bv(z3::expr e, unsigned size_bits) {
    const unsigned width = size_bits == 0 ? 64U : size_bits;

    if (e.is_bool()) {
        return z3::ite(e, ctx_.bv_val(1, width), ctx_.bv_val(0, width));
    }
    if (e.is_bv()) {
        unsigned curr_sz = e.get_sort().bv_size();
        if (curr_sz < width) {
            return z3::zext(e, width - curr_sz);
        }
        if (curr_sz > width) {
            return e.extract(width - 1, 0);
        }
        return e;
    }
    return fresh_bv(width, "coerce_bv");
}

LogicCondition Z3Converter::convert_to_condition(aletheia::DataflowObject* obj) {
    return LogicCondition(ensure_bool(convert(obj)));
}

z3::expr Z3Converter::convert(aletheia::DataflowObject* obj) {
    if (!obj) {
        return fresh_bool("null_obj");
    }
    
    if (auto* c = aletheia::dyn_cast<aletheia::Constant>(obj)) {
        return convert_constant(c);
    } else if (auto* v = aletheia::dyn_cast<aletheia::Variable>(obj)) {
        return convert_variable(v);
    } else if (auto* o = aletheia::dyn_cast<aletheia::Operation>(obj)) {
        return convert_operation(o);
    }
    
    return fresh_bv(normalized_bit_width(obj->size_bytes), "unknown_obj");
}

z3::expr Z3Converter::convert_constant(aletheia::Constant* c) {
    return ctx_.bv_val(c->value(), normalized_bit_width(c->size_bytes));
}

z3::expr Z3Converter::convert_variable(aletheia::Variable* v) {
    if (v->name().empty()) {
        return fresh_bv(normalized_bit_width(v->size_bytes), "anon_var");
    }
    return ctx_.bv_const(v->name().c_str(), normalized_bit_width(v->size_bytes));
}

z3::expr Z3Converter::convert_operation(aletheia::Operation* o) {
    const unsigned result_width = normalized_bit_width(o->size_bytes);
    const bool bool_result = is_boolean_operation(o->type());
    const auto& ops = o->operands();

    if (ops.empty()) {
        return bool_result ? fresh_bool("op_empty_bool") : fresh_bv(result_width, "op_empty_bv");
    }

    auto bv_at = [&](std::size_t index, unsigned width = 0U) {
        const unsigned w = width == 0U ? result_width : width;
        if (index >= ops.size()) {
            return fresh_bv(w, "missing_bv");
        }
        return ensure_bv(convert(ops[index]), w);
    };

    auto bool_at = [&](std::size_t index) {
        if (index >= ops.size()) {
            return fresh_bool("missing_bool");
        }
        return ensure_bool(convert(ops[index]));
    };

    using aletheia::OperationType;
    switch (o->type()) {
        case OperationType::deref:
        case OperationType::address_of:
        case OperationType::member_access:
        case OperationType::field:
        case OperationType::pointer:
        case OperationType::call:
        case OperationType::list_op:
        case OperationType::unknown: {
            std::string fp = expr_fingerprint(o);
            std::string name = std::string("opaque_op_") + std::to_string(std::hash<std::string>{}(fp));
            return ctx_.bv_const(name.c_str(), result_width);
        }

        case OperationType::cast:
        case OperationType::low:
            return bv_at(0);

        case OperationType::ternary: {
            if (ops.size() < 3) {
                return fresh_bv(result_width, "ternary_short");
            }
            auto cond = bool_at(0);
            auto t = bv_at(1);
            auto f = bv_at(2);
            return z3::ite(cond, t, f);
        }

        case OperationType::logical_and: {
            z3::expr acc = bool_at(0);
            for (std::size_t i = 1; i < ops.size(); ++i) {
                acc = acc && bool_at(i);
            }
            return acc;
        }
        case OperationType::logical_or: {
            z3::expr acc = bool_at(0);
            for (std::size_t i = 1; i < ops.size(); ++i) {
                acc = acc || bool_at(i);
            }
            return acc;
        }
        case OperationType::logical_not:
            return !bool_at(0);

        case OperationType::eq:
            return bv_at(0) == bv_at(1);
        case OperationType::neq:
            return bv_at(0) != bv_at(1);
        case OperationType::lt:
            return z3::slt(bv_at(0), bv_at(1));
        case OperationType::le:
            return z3::sle(bv_at(0), bv_at(1));
        case OperationType::gt:
            return z3::sgt(bv_at(0), bv_at(1));
        case OperationType::ge:
            return z3::sge(bv_at(0), bv_at(1));
        case OperationType::lt_us:
            return z3::ult(bv_at(0), bv_at(1));
        case OperationType::le_us:
            return z3::ule(bv_at(0), bv_at(1));
        case OperationType::gt_us:
            return z3::ugt(bv_at(0), bv_at(1));
        case OperationType::ge_us:
            return z3::uge(bv_at(0), bv_at(1));

        case OperationType::add:
        case OperationType::add_float:
            return bv_at(0) + bv_at(1);
        case OperationType::sub:
        case OperationType::sub_float:
            return bv_at(0) - bv_at(1);
        case OperationType::mul:
        case OperationType::mul_us:
        case OperationType::mul_float:
            return bv_at(0) * bv_at(1);
        case OperationType::div:
        case OperationType::div_float:
            return bv_at(0) / bv_at(1);
        case OperationType::div_us:
            return z3::udiv(bv_at(0), bv_at(1));
        case OperationType::mod:
            return z3::srem(bv_at(0), bv_at(1));
        case OperationType::mod_us:
            return z3::urem(bv_at(0), bv_at(1));
        case OperationType::power:
            return fresh_bv(result_width, "pow_op");
        case OperationType::negate:
            return -bv_at(0);

        case OperationType::add_with_carry:
        case OperationType::adc: {
            z3::expr sum = bv_at(0) + bv_at(1);
            if (ops.size() >= 3) {
                sum = sum + bv_at(2);
            }
            return sum;
        }
        case OperationType::sub_with_carry: {
            z3::expr diff = bv_at(0) - bv_at(1);
            if (ops.size() >= 3) {
                diff = diff - bv_at(2);
            }
            return diff;
        }

        case OperationType::bit_and:
            return bv_at(0) & bv_at(1);
        case OperationType::bit_or:
            return bv_at(0) | bv_at(1);
        case OperationType::bit_xor:
            return bv_at(0) ^ bv_at(1);
        case OperationType::bit_not:
            return ~bv_at(0);

        case OperationType::shl:
        case OperationType::left_rotate:
        case OperationType::left_rotate_carry:
            return z3::shl(bv_at(0), bv_at(1));
        case OperationType::shr:
        case OperationType::sar:
        case OperationType::right_rotate:
        case OperationType::right_rotate_carry:
            return z3::ashr(bv_at(0), bv_at(1));
        case OperationType::shr_us:
            return z3::lshr(bv_at(0), bv_at(1));
    }

    // Conservative default for unsupported operations: unconstrained symbolic
    // value (never a hard `false`) so we do not over-prune paths.
    return bool_result ? fresh_bool("unsupported_bool") : fresh_bv(result_width, "unsupported_bv");
}

} // namespace logos
