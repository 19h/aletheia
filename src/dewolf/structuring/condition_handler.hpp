#pragma once

#include "../../dewolf_logic/z3_logic.hpp"
#include "../structures/dataflow.hpp"
#include <optional>
#include <string>
#include <unordered_map>

namespace dewolf {

struct CaseNodeProperties {
    Expression* expression = nullptr;
    Constant* constant = nullptr;
    bool negated = false;
};

class ConditionHandler {
public:
    explicit ConditionHandler(z3::context& ctx) : ctx_(ctx) {}

    dewolf_logic::LogicCondition add_condition(Condition* condition) {
        if (!condition) {
            return dewolf_logic::LogicCondition(ctx_.bool_val(true));
        }

        if (auto it = condition_to_symbol_.find(condition); it != condition_to_symbol_.end()) {
            return dewolf_logic::LogicCondition(ctx_.bool_const(it->second.c_str()));
        }

        const std::string symbol = "cond_" + std::to_string(next_symbol_id_++);
        condition_to_symbol_.emplace(condition, symbol);
        symbol_to_condition_.emplace(symbol, condition);
        maybe_record_case_properties(symbol, condition);
        return dewolf_logic::LogicCondition(ctx_.bool_const(symbol.c_str()));
    }

    std::optional<std::string> symbol_for_condition(const Condition* condition) const {
        if (!condition) return std::nullopt;
        auto it = condition_to_symbol_.find(const_cast<Condition*>(condition));
        if (it == condition_to_symbol_.end()) return std::nullopt;
        return it->second;
    }

    Condition* condition_for_symbol(const std::string& symbol) const {
        auto it = symbol_to_condition_.find(symbol);
        if (it == symbol_to_condition_.end()) return nullptr;
        return it->second;
    }

    std::optional<CaseNodeProperties> case_properties_for_symbol(const std::string& symbol) const {
        auto it = case_properties_.find(symbol);
        if (it == case_properties_.end()) return std::nullopt;
        return it->second;
    }

    const std::unordered_map<Condition*, std::string>& condition_to_symbol() const {
        return condition_to_symbol_;
    }

    const std::unordered_map<std::string, Condition*>& symbol_to_condition() const {
        return symbol_to_condition_;
    }

private:
    void maybe_record_case_properties(const std::string& symbol, Condition* condition) {
        if (!condition) return;
        if (condition->type() != OperationType::eq && condition->type() != OperationType::neq) {
            return;
        }

        Expression* lhs = condition->lhs();
        Expression* rhs = condition->rhs();

        auto* lhs_const = dynamic_cast<Constant*>(lhs);
        auto* rhs_const = dynamic_cast<Constant*>(rhs);

        if (lhs_const && !rhs_const) {
            case_properties_[symbol] = {rhs, lhs_const, condition->type() == OperationType::neq};
            return;
        }
        if (rhs_const && !lhs_const) {
            case_properties_[symbol] = {lhs, rhs_const, condition->type() == OperationType::neq};
        }
    }

    z3::context& ctx_;
    std::size_t next_symbol_id_ = 0;
    std::unordered_map<Condition*, std::string> condition_to_symbol_;
    std::unordered_map<std::string, Condition*> symbol_to_condition_;
    std::unordered_map<std::string, CaseNodeProperties> case_properties_;
};

} // namespace dewolf
