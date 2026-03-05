#include "ir_query.hpp"
#include <unordered_set>

namespace aletheia::debug {

std::vector<const Variable*> IrQuery::find_variables(
    const ControlFlowGraph* cfg,
    std::function<bool(const Variable*)> predicate) {
    std::vector<const Variable*> result;
    if (!cfg) return result;

    std::unordered_set<const Variable*> seen;
    for (auto* block : cfg->blocks()) {
        for (auto* inst : block->instructions()) {
            std::unordered_set<Variable*> defs;
            inst->collect_definitions(defs);
            for (auto* v : defs) {
                if (!seen.contains(v) && predicate(v)) {
                    seen.insert(v);
                    result.push_back(v);
                }
            }
            std::unordered_set<Variable*> reqs;
            inst->collect_requirements(reqs);
            for (auto* v : reqs) {
                if (!seen.contains(v) && predicate(v)) {
                    seen.insert(v);
                    result.push_back(v);
                }
            }
        }
    }
    return result;
}

std::vector<std::pair<const BasicBlock*, const Instruction*>> IrQuery::find_instructions(
    const ControlFlowGraph* cfg,
    std::function<bool(const Instruction*)> predicate) {
    std::vector<std::pair<const BasicBlock*, const Instruction*>> result;
    if (!cfg) return result;

    for (auto* block : cfg->blocks()) {
        for (auto* inst : block->instructions()) {
            if (predicate(inst)) {
                result.emplace_back(block, inst);
            }
        }
    }
    return result;
}

std::vector<std::pair<const BasicBlock*, const Instruction*>> IrQuery::find_definitions(
    const ControlFlowGraph* cfg, const std::string& var_name) {
    return find_instructions(cfg, [&var_name](const Instruction* inst) {
        std::unordered_set<Variable*> defs;
        inst->collect_definitions(defs);
        for (auto* v : defs) {
            if (v->name() == var_name) return true;
        }
        return false;
    });
}

std::vector<std::pair<const BasicBlock*, const Instruction*>> IrQuery::find_uses(
    const ControlFlowGraph* cfg, const std::string& var_name) {
    return find_instructions(cfg, [&var_name](const Instruction* inst) {
        std::unordered_set<Variable*> reqs;
        inst->collect_requirements(reqs);
        for (auto* v : reqs) {
            if (v->name() == var_name) return true;
        }
        return false;
    });
}

} // namespace aletheia::debug
