#include "preprocessing_stages.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace dewolf {

namespace {

std::optional<OperationType> operation_type_from_idiom(const std::string& operation) {
    if (operation == "division") {
        return OperationType::div;
    }
    if (operation == "division unsigned") {
        return OperationType::div_us;
    }
    if (operation == "modulo") {
        return OperationType::mod;
    }
    if (operation == "modulo unsigned") {
        return OperationType::mod_us;
    }
    if (operation == "multiplication") {
        return OperationType::mul;
    }
    return std::nullopt;
}

Variable* first_defined_variable(Instruction* inst) {
    if (inst == nullptr) {
        return nullptr;
    }
    auto defs = inst->definitions();
    if (!defs.empty()) {
        return defs.front();
    }
    return nullptr;
}

Variable* find_named_variable_in_window(const std::vector<Instruction*>& window, const std::string& name) {
    for (Instruction* inst : window) {
        for (Variable* def : inst->definitions()) {
            if (def != nullptr && def->name() == name) {
                return def;
            }
        }
        for (Variable* req : inst->requirements()) {
            if (req != nullptr && req->name() == name) {
                return req;
            }
        }
    }
    return nullptr;
}

bool window_has_unhandled_instructions(const std::vector<Instruction*>& insts, size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
        Instruction* inst = insts[i];
        if (inst == nullptr) {
            return true;
        }
        if (is_branch(inst) || is_return(inst) || is_phi(inst)) {
            return true;
        }
        if (dynamic_cast<BreakInstr*>(inst) != nullptr || dynamic_cast<ContinueInstr*>(inst) != nullptr) {
            return true;
        }
    }
    return false;
}

Expression* constant_from_idiom(DecompilerTask& task, int64_t value, size_t size_bytes) {
    if (value >= 0) {
        return task.arena().create<Constant>(static_cast<uint64_t>(value), size_bytes);
    }

    const auto abs_value = static_cast<uint64_t>(-(value + 1)) + 1;
    auto* positive = task.arena().create<Constant>(abs_value, size_bytes);
    return task.arena().create<Operation>(OperationType::negate, std::vector<Expression*>{positive}, size_bytes);
}

Instruction* synthesize_idiom_instruction(DecompilerTask& task,
                                          const dewolf_idioms::IdiomTag& tag,
                                          const std::vector<Instruction*>& window) {
    auto op_type = operation_type_from_idiom(tag.operation);
    if (!op_type.has_value()) {
        return nullptr;
    }

    Variable* operand_var = nullptr;
    if (!tag.operand.empty()) {
        operand_var = find_named_variable_in_window(window, tag.operand);
    }
    if (operand_var == nullptr && !window.empty()) {
        operand_var = first_defined_variable(window.front());
    }

    const size_t width = (operand_var != nullptr && operand_var->size_bytes > 0)
        ? operand_var->size_bytes
        : 8;

    if (operand_var == nullptr) {
        const std::string fallback_name = tag.operand.empty() ? "idiom_tmp" : tag.operand;
        operand_var = task.arena().create<Variable>(fallback_name, width);
    }

    Expression* rhs_operand = operand_var;
    Expression* rhs_constant = constant_from_idiom(task, tag.constant, width);
    auto* recovered = task.arena().create<Operation>(
        *op_type,
        std::vector<Expression*>{rhs_operand, rhs_constant},
        width);

    auto* assign = task.arena().create<Assignment>(operand_var, recovered);
    assign->set_address(tag.address);
    return assign;
}

struct RegisterPairSpec {
    const char* high;
    const char* low;
    const char* pair;
    uint64_t shift_bits;
    size_t pair_size_bytes;
};

constexpr std::array<RegisterPairSpec, 3> kRegisterPairs = {
    RegisterPairSpec{"edx", "eax", "edx_eax_pair", 32, 8},
    RegisterPairSpec{"dx", "ax", "dx_ax_pair", 16, 4},
    RegisterPairSpec{"rdx", "rax", "rdx_rax_pair", 64, 16},
};

Expression* strip_cast(Expression* expr) {
    while (auto* op = dynamic_cast<Operation*>(expr)) {
        if (op->type() != OperationType::cast || op->operands().size() != 1 || op->operands()[0] == nullptr) {
            break;
        }
        expr = op->operands()[0];
    }
    return expr;
}

Variable* match_low_register(Expression* expr, const RegisterPairSpec& spec) {
    auto* v = dynamic_cast<Variable*>(strip_cast(expr));
    if (v != nullptr && v->name() == spec.low) {
        return v;
    }
    return nullptr;
}

Variable* match_shifted_high_register(Expression* expr, const RegisterPairSpec& spec) {
    auto* op = dynamic_cast<Operation*>(strip_cast(expr));
    if (op == nullptr || op->type() != OperationType::shl || op->operands().size() != 2) {
        return nullptr;
    }

    auto* high = dynamic_cast<Variable*>(strip_cast(op->operands()[0]));
    auto* shift = dynamic_cast<Constant*>(strip_cast(op->operands()[1]));
    if (high == nullptr || shift == nullptr) {
        return nullptr;
    }

    if (high->name() != spec.high || shift->value() != spec.shift_bits) {
        return nullptr;
    }
    return high;
}

Expression* rewrite_register_pair_concat(DecompilerTask& task, Operation* op) {
    if (op == nullptr) {
        return nullptr;
    }
    if ((op->type() != OperationType::add && op->type() != OperationType::bit_or) || op->operands().size() != 2) {
        return nullptr;
    }

    Expression* lhs = op->operands()[0];
    Expression* rhs = op->operands()[1];

    for (const auto& spec : kRegisterPairs) {
        Variable* high = match_shifted_high_register(lhs, spec);
        Variable* low = match_low_register(rhs, spec);
        if (high == nullptr || low == nullptr) {
            high = match_shifted_high_register(rhs, spec);
            low = match_low_register(lhs, spec);
        }

        if (high == nullptr || low == nullptr) {
            continue;
        }

        auto* pair = task.arena().create<Variable>(spec.pair, spec.pair_size_bytes);
        pair->set_aliased(high->is_aliased() || low->is_aliased());
        pair->set_ir_type(std::make_shared<Integer>(spec.shift_bits * 2, false));
        return pair;
    }

    return nullptr;
}

Expression* rewrite_register_pair_expression(DecompilerTask& task, Expression* expr, bool& changed) {
    if (expr == nullptr) {
        return nullptr;
    }

    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression*& item : list->mutable_operands()) {
            Expression* rewritten = rewrite_register_pair_expression(task, item, changed);
            if (rewritten != item && rewritten != nullptr) {
                item = rewritten;
                changed = true;
            }
        }
        return expr;
    }

    auto* op = dynamic_cast<Operation*>(expr);
    if (op == nullptr) {
        return expr;
    }

    for (Expression*& item : op->mutable_operands()) {
        Expression* rewritten = rewrite_register_pair_expression(task, item, changed);
        if (rewritten != item && rewritten != nullptr) {
            item = rewritten;
            changed = true;
        }
    }

    if (Expression* merged = rewrite_register_pair_concat(task, op); merged != nullptr) {
        changed = true;
        return merged;
    }

    return expr;
}

void rewrite_register_pairs_in_instruction(DecompilerTask& task, Instruction* inst, bool& changed) {
    if (inst == nullptr) {
        return;
    }

    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
        Expression* new_dest = rewrite_register_pair_expression(task, assign->destination(), changed);
        Expression* new_val = rewrite_register_pair_expression(task, assign->value(), changed);
        if (new_dest != assign->destination()) {
            assign->set_destination(new_dest);
            changed = true;
        }
        if (new_val != assign->value()) {
            assign->set_value(new_val);
            changed = true;
        }
        return;
    }

    if (auto* branch = dynamic_cast<Branch*>(inst)) {
        Expression* rewritten = rewrite_register_pair_expression(task, branch->condition(), changed);
        if (rewritten != branch->condition()) {
            if (auto* cond = dynamic_cast<Condition*>(rewritten)) {
                branch->set_condition(cond);
                changed = true;
            }
        }
        return;
    }

    if (auto* ib = dynamic_cast<IndirectBranch*>(inst)) {
        Expression* before = ib->expression();
        Expression* after = rewrite_register_pair_expression(task, before, changed);
        if (after != before && after != nullptr) {
            ib->substitute(before, after);
            changed = true;
        }
        return;
    }

    if (auto* ret = dynamic_cast<Return*>(inst)) {
        for (Expression* value : ret->values()) {
            Expression* rewritten = rewrite_register_pair_expression(task, value, changed);
            if (rewritten != value && rewritten != nullptr) {
                ret->substitute(value, rewritten);
                changed = true;
            }
        }
        return;
    }
}

} // namespace

void CompilerIdiomHandlingStage::execute(DecompilerTask& task) {
    if (!task.cfg() || task.idiom_tags().empty()) {
        return;
    }

    std::unordered_map<ida::Address, std::vector<dewolf_idioms::IdiomTag>> tags_by_address;
    for (const auto& tag : task.idiom_tags()) {
        if (tag.length == 0) {
            continue;
        }
        tags_by_address[tag.address].push_back(tag);
    }
    for (auto& [address, tags] : tags_by_address) {
        (void)address;
        std::sort(tags.begin(), tags.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.length > rhs.length;
        });
    }

    std::unordered_set<ida::Address> consumed_tags;

    for (BasicBlock* block : task.cfg()->blocks()) {
        auto instructions = block->instructions();
        if (instructions.empty()) {
            continue;
        }

        bool block_changed = false;
        for (size_t i = 0; i < instructions.size(); ++i) {
            Instruction* start_inst = instructions[i];
            if (start_inst == nullptr) {
                continue;
            }

            const ida::Address start_address = start_inst->address();
            if (consumed_tags.contains(start_address)) {
                continue;
            }

            auto it = tags_by_address.find(start_address);
            if (it == tags_by_address.end()) {
                continue;
            }

            bool applied = false;
            for (const auto& tag : it->second) {
                const size_t end = i + tag.length;
                if (end > instructions.size()) {
                    continue;
                }
                if (window_has_unhandled_instructions(instructions, i, end)) {
                    continue;
                }

                std::vector<Instruction*> window(
                    instructions.begin() + static_cast<ptrdiff_t>(i),
                    instructions.begin() + static_cast<ptrdiff_t>(end));

                Instruction* replacement = synthesize_idiom_instruction(task, tag, window);
                if (replacement == nullptr) {
                    continue;
                }

                std::vector<Instruction*> rewritten;
                rewritten.reserve(instructions.size() - tag.length + 1);
                rewritten.insert(
                    rewritten.end(),
                    instructions.begin(),
                    instructions.begin() + static_cast<ptrdiff_t>(i));
                rewritten.push_back(replacement);
                rewritten.insert(
                    rewritten.end(),
                    instructions.begin() + static_cast<ptrdiff_t>(end),
                    instructions.end());

                instructions = std::move(rewritten);
                consumed_tags.insert(start_address);
                block_changed = true;
                applied = true;
                break;
            }

            if (applied && i < instructions.size()) {
                // Continue scanning after the replacement instruction.
                continue;
            }
        }

        if (block_changed) {
            block->set_instructions(std::move(instructions));
        }
    }
}

void RegisterPairHandlingStage::execute(DecompilerTask& task) {
    if (!task.cfg()) {
        return;
    }

    for (BasicBlock* block : task.cfg()->blocks()) {
        bool changed = false;
        auto instructions = block->instructions();
        for (Instruction* inst : instructions) {
            rewrite_register_pairs_in_instruction(task, inst, changed);
        }
        if (changed) {
            block->set_instructions(std::move(instructions));
        }
    }
}

void SwitchVariableDetectionStage::execute(DecompilerTask& task) {
    // Detect switch statements based on indirect jumps and known patterns
}

void RemoveGoPrologueStage::execute(DecompilerTask& task) {
    // Pattern match and remove Go runtime stack check prologues
}

void RemoveStackCanaryStage::execute(DecompilerTask& task) {
    // Identify and remove stack canary setup and check sequences
}

} // namespace dewolf
