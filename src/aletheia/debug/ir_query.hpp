#pragma once
#include "../structures/cfg.hpp"
#include <functional>
#include <string>
#include <vector>
#include <utility>

namespace aletheia::debug {

class IrQuery {
public:
    static std::vector<const Variable*> find_variables(
        const ControlFlowGraph* cfg,
        std::function<bool(const Variable*)> predicate);

    static std::vector<std::pair<const BasicBlock*, const Instruction*>> find_instructions(
        const ControlFlowGraph* cfg,
        std::function<bool(const Instruction*)> predicate);

    static std::vector<std::pair<const BasicBlock*, const Instruction*>> find_definitions(
        const ControlFlowGraph* cfg, const std::string& var_name);

    static std::vector<std::pair<const BasicBlock*, const Instruction*>> find_uses(
        const ControlFlowGraph* cfg, const std::string& var_name);
};

} // namespace aletheia::debug
