#include "preprocessing_stages.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <optional>
#include <string>
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

struct VarKey {
    std::string name;
    std::size_t version{};

    bool operator==(const VarKey& other) const {
        return name == other.name && version == other.version;
    }
};

struct VarKeyHash {
    std::size_t operator()(const VarKey& key) const {
        std::size_t h1 = std::hash<std::string>{}(key.name);
        std::size_t h2 = std::hash<std::size_t>{}(key.version);
        return h1 ^ (h2 << 1);
    }
};

VarKey var_key(const Variable* var) {
    return VarKey{var->name(), var->ssa_version()};
}

using DefMap = std::unordered_map<VarKey, Assignment*, VarKeyHash>;
using UseMap = std::unordered_map<VarKey, std::vector<Instruction*>, VarKeyHash>;

Variable* as_variable_ignoring_cast(Expression* expr) {
    Expression* current = strip_cast(expr);
    return dynamic_cast<Variable*>(current);
}

std::string expression_fingerprint(Expression* expr) {
    if (expr == nullptr) {
        return "null";
    }
    if (auto* c = dynamic_cast<Constant*>(expr)) {
        return "c:" + std::to_string(c->value()) + ":" + std::to_string(c->size_bytes);
    }
    if (auto* v = dynamic_cast<Variable*>(expr)) {
        return "v:" + v->name() + "#" + std::to_string(v->ssa_version());
    }
    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        std::string out = "l(";
        bool first = true;
        for (Expression* item : list->operands()) {
            if (!first) {
                out += ",";
            }
            first = false;
            out += expression_fingerprint(item);
        }
        out += ")";
        return out;
    }
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        std::string out = "o:" + std::to_string(static_cast<int>(op->type())) + "(";
        bool first = true;
        for (Expression* item : op->operands()) {
            if (!first) {
                out += ",";
            }
            first = false;
            out += expression_fingerprint(item);
        }
        out += ")";
        return out;
    }
    return "expr";
}

void collect_deref_fingerprints(Expression* expr, std::unordered_set<std::string>& out) {
    if (expr == nullptr) {
        return;
    }
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        if (op->type() == OperationType::deref) {
            out.insert(expression_fingerprint(expr));
        }
        for (Expression* item : op->operands()) {
            collect_deref_fingerprints(item, out);
        }
        return;
    }
    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression* item : list->operands()) {
            collect_deref_fingerprints(item, out);
        }
    }
}

void collect_variables(Expression* expr,
                       std::vector<Variable*>& out,
                       std::unordered_set<VarKey, VarKeyHash>& seen) {
    if (expr == nullptr) {
        return;
    }
    if (auto* var = dynamic_cast<Variable*>(expr)) {
        VarKey key = var_key(var);
        if (seen.insert(key).second) {
            out.push_back(var);
        }
        return;
    }
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        for (Expression* item : op->operands()) {
            collect_variables(item, out, seen);
        }
        return;
    }
    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression* item : list->operands()) {
            collect_variables(item, out, seen);
        }
    }
}

Variable* first_variable(Expression* expr) {
    std::vector<Variable*> vars;
    std::unordered_set<VarKey, VarKeyHash> seen;
    collect_variables(expr, vars, seen);
    if (!vars.empty()) {
        return vars.front();
    }
    return nullptr;
}

void build_def_use_maps(ControlFlowGraph* cfg,
                        DefMap& def_map,
                        UseMap& use_map,
                        std::unordered_set<std::string>& branch_dereferences) {
    for (BasicBlock* block : cfg->blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                if (auto* dst = dynamic_cast<Variable*>(assign->destination())) {
                    def_map[var_key(dst)] = assign;
                }
            }

            for (Variable* req : inst->requirements()) {
                use_map[var_key(req)].push_back(inst);
            }

            if (auto* branch = dynamic_cast<Branch*>(inst)) {
                collect_deref_fingerprints(branch->condition(), branch_dereferences);
            }
        }
    }
}

bool instruction_requires_single_var(Instruction* inst, const VarKey& key) {
    auto req = inst->requirements();
    if (req.size() != 1 || req[0] == nullptr) {
        return false;
    }
    return var_key(req[0]) == key;
}

bool is_copy_assigned(const Variable* value, const DefMap& def_map) {
    auto it = def_map.find(var_key(value));
    if (it == def_map.end() || it->second == nullptr) {
        return false;
    }
    return as_variable_ignoring_cast(it->second->value()) != nullptr;
}

bool is_used_in_condition_assignment(const Variable* value, const UseMap& use_map) {
    auto it = use_map.find(var_key(value));
    if (it == use_map.end()) {
        return false;
    }
    for (Instruction* use : it->second) {
        auto* assign = dynamic_cast<Assignment*>(use);
        if (assign == nullptr) {
            continue;
        }
        if (dynamic_cast<Condition*>(assign->value()) == nullptr) {
            continue;
        }
        if (instruction_requires_single_var(assign, var_key(value))) {
            return true;
        }
    }
    return false;
}

bool is_used_in_branch(const Variable* value, const UseMap& use_map) {
    auto it = use_map.find(var_key(value));
    if (it == use_map.end()) {
        return false;
    }
    for (Instruction* use : it->second) {
        auto* branch = dynamic_cast<Branch*>(use);
        if (branch == nullptr) {
            continue;
        }
        if (instruction_requires_single_var(branch, var_key(value))) {
            return true;
        }
    }
    return false;
}

bool is_predecessor_dereferenced_in_branch(const Variable* value,
                                           const DefMap& def_map,
                                           const std::unordered_set<std::string>& branch_dereferences) {
    auto it = def_map.find(var_key(value));
    if (it == def_map.end() || it->second == nullptr) {
        return false;
    }

    Expression* rhs = it->second->value();
    if (rhs == nullptr) {
        return false;
    }

    if (branch_dereferences.contains(expression_fingerprint(rhs))) {
        return true;
    }

    std::unordered_set<std::string> def_dereferences;
    collect_deref_fingerprints(rhs, def_dereferences);
    for (const std::string& fp : def_dereferences) {
        if (branch_dereferences.contains(fp)) {
            return true;
        }
    }
    return false;
}

bool is_bounds_checked(const Variable* value,
                       const DefMap& def_map,
                       const UseMap& use_map,
                       const std::unordered_set<std::string>& branch_dereferences) {
    return is_copy_assigned(value, def_map)
        || is_used_in_condition_assignment(value, use_map)
        || is_used_in_branch(value, use_map)
        || is_predecessor_dereferenced_in_branch(value, def_map, branch_dereferences);
}

Variable* find_switch_candidate(Variable* start,
                                const DefMap& def_map,
                                const UseMap& use_map,
                                const std::unordered_set<std::string>& branch_dereferences) {
    if (start == nullptr) {
        return nullptr;
    }

    std::deque<Variable*> todo;
    std::unordered_set<VarKey, VarKeyHash> visited;
    todo.push_back(start);

    while (!todo.empty()) {
        Variable* current = todo.front();
        todo.pop_front();
        if (current == nullptr) {
            continue;
        }

        VarKey key = var_key(current);
        if (!visited.insert(key).second) {
            continue;
        }

        if (is_bounds_checked(current, def_map, use_map, branch_dereferences)) {
            return current;
        }

        auto it = def_map.find(key);
        if (it == def_map.end() || it->second == nullptr || it->second->value() == nullptr) {
            continue;
        }

        std::vector<Variable*> predecessors;
        std::unordered_set<VarKey, VarKeyHash> seen;
        collect_variables(it->second->value(), predecessors, seen);
        for (Variable* pred : predecessors) {
            if (pred != nullptr && !visited.contains(var_key(pred))) {
                todo.push_back(pred);
            }
        }
    }

    return nullptr;
}

bool is_switch_block(BasicBlock* block) {
    bool has_indirect = !block->instructions().empty()
        && dynamic_cast<IndirectBranch*>(block->instructions().back()) != nullptr;
    bool has_switch_edge = std::any_of(
        block->successors().begin(),
        block->successors().end(),
        [](Edge* e) { return e != nullptr && e->type() == EdgeType::Switch; });
    return has_switch_edge || (has_indirect && block->successors().size() > 1);
}

std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool constant_matches_signed(const Constant* constant, int64_t target) {
    if (constant == nullptr) {
        return false;
    }
    const int64_t as_signed = static_cast<int64_t>(constant->value());
    return as_signed == target;
}

bool expression_has_constant(Expression* expr, int64_t target) {
    if (expr == nullptr) {
        return false;
    }
    if (auto* c = dynamic_cast<Constant*>(expr)) {
        return constant_matches_signed(c, target);
    }
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        for (Expression* child : op->operands()) {
            if (expression_has_constant(child, target)) {
                return true;
            }
        }
    }
    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression* child : list->operands()) {
            if (expression_has_constant(child, target)) {
                return true;
            }
        }
    }
    return false;
}

bool expression_has_variable_prefix(Expression* expr, const std::string& prefix) {
    if (expr == nullptr) {
        return false;
    }
    if (auto* v = dynamic_cast<Variable*>(expr)) {
        return to_lower_ascii(v->name()).starts_with(prefix);
    }
    if (auto* op = dynamic_cast<Operation*>(expr)) {
        for (Expression* child : op->operands()) {
            if (expression_has_variable_prefix(child, prefix)) {
                return true;
            }
        }
    }
    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression* child : list->operands()) {
            if (expression_has_variable_prefix(child, prefix)) {
                return true;
            }
        }
    }
    return false;
}

bool expression_is_addressish(Expression* expr) {
    if (expr == nullptr) {
        return false;
    }
    Expression* base = strip_cast(expr);
    if (auto* op = dynamic_cast<Operation*>(base)) {
        if (op->type() == OperationType::address_of) {
            return true;
        }
    }
    if (auto* v = dynamic_cast<Variable*>(base)) {
        const std::string name = to_lower_ascii(v->name());
        return name.find("return") != std::string::npos;
    }
    return false;
}

bool expression_matches_go_stack_guard_rhs(Expression* expr) {
    if (expr == nullptr) {
        return false;
    }

    // Pattern group #1: *(r14 + 0x10)
    if (expression_has_variable_prefix(expr, "r14") && expression_has_constant(expr, 0x10)) {
        return true;
    }

    // Pattern group #2: *(*(... fsbase/gsbase ...)+offset)
    const bool has_fsbase = expression_has_variable_prefix(expr, "fsbase");
    const bool has_gsbase = expression_has_variable_prefix(expr, "gsbase");
    if (has_fsbase || has_gsbase) {
        constexpr std::array<int64_t, 11> kKnownOffsets = {
            0, -4, 0x8, -8, 0x10, 0x14, 0x18, 0x28, 0x30, 0x468, 0x8A0
        };
        for (int64_t offset : kKnownOffsets) {
            if (expression_has_constant(expr, offset)) {
                return true;
            }
        }
    }

    return false;
}

bool branch_condition_matches_go_root_guard(Branch* branch) {
    if (branch == nullptr || branch->condition() == nullptr) {
        return false;
    }

    Condition* cond = branch->condition();
    const OperationType type = cond->type();
    const bool is_rel = type == OperationType::lt || type == OperationType::le || type == OperationType::gt
        || type == OperationType::ge || type == OperationType::lt_us || type == OperationType::le_us
        || type == OperationType::gt_us || type == OperationType::ge_us;
    if (!is_rel) {
        return false;
    }

    Expression* lhs = cond->lhs();
    Expression* rhs = cond->rhs();

    return (expression_is_addressish(lhs) && expression_matches_go_stack_guard_rhs(rhs))
        || (expression_is_addressish(rhs) && expression_matches_go_stack_guard_rhs(lhs));
}

bool expression_contains_morestack_call(Expression* expr) {
    if (expr == nullptr) {
        return false;
    }

    if (auto* call = dynamic_cast<Call*>(expr)) {
        if (auto* target_var = dynamic_cast<Variable*>(call->target())) {
            return to_lower_ascii(target_var->name()).find("morestack") != std::string::npos;
        }
        return false;
    }

    if (auto* op = dynamic_cast<Operation*>(expr)) {
        if (op->type() == OperationType::call && !op->operands().empty()) {
            if (auto* target_var = dynamic_cast<Variable*>(op->operands()[0])) {
                if (to_lower_ascii(target_var->name()).find("morestack") != std::string::npos) {
                    return true;
                }
            }
        }
        for (Expression* child : op->operands()) {
            if (expression_contains_morestack_call(child)) {
                return true;
            }
        }
    }

    if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (Expression* child : list->operands()) {
            if (expression_contains_morestack_call(child)) {
                return true;
            }
        }
    }

    return false;
}

bool block_contains_morestack_call(BasicBlock* block) {
    if (block == nullptr) {
        return false;
    }
    for (Instruction* inst : block->instructions()) {
        if (auto* assign = dynamic_cast<Assignment*>(inst)) {
            if (expression_contains_morestack_call(assign->value())) {
                return true;
            }
        }
    }
    return false;
}

bool successor_path_contains_morestack(BasicBlock* start, BasicBlock* root) {
    if (start == nullptr) {
        return false;
    }

    std::vector<BasicBlock*> stack{start};
    std::unordered_set<BasicBlock*> visited;

    while (!stack.empty()) {
        BasicBlock* node = stack.back();
        stack.pop_back();
        if (node == nullptr || visited.contains(node)) {
            continue;
        }
        visited.insert(node);

        if (block_contains_morestack_call(node)) {
            return true;
        }

        for (Edge* edge : node->successors()) {
            if (!edge || !edge->target()) {
                continue;
            }
            BasicBlock* succ = edge->target();
            if (succ == root || !visited.contains(succ)) {
                stack.push_back(succ);
            }
        }
    }

    return false;
}

void remove_unreachable_blocks(ControlFlowGraph* cfg) {
    if (cfg == nullptr || cfg->entry_block() == nullptr) {
        return;
    }

    std::vector<BasicBlock*> stack{cfg->entry_block()};
    std::unordered_set<BasicBlock*> reachable;
    while (!stack.empty()) {
        BasicBlock* node = stack.back();
        stack.pop_back();
        if (node == nullptr || reachable.contains(node)) {
            continue;
        }
        reachable.insert(node);
        for (Edge* edge : node->successors()) {
            if (edge != nullptr && edge->target() != nullptr) {
                stack.push_back(edge->target());
            }
        }
    }

    std::unordered_set<BasicBlock*> dead;
    for (BasicBlock* block : cfg->blocks()) {
        if (!reachable.contains(block)) {
            dead.insert(block);
        }
    }
    if (!dead.empty()) {
        cfg->remove_nodes_from(dead);
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
    if (!task.cfg()) {
        return;
    }

    DefMap def_map;
    UseMap use_map;
    std::unordered_set<std::string> branch_dereferences;
    build_def_use_maps(task.cfg(), def_map, use_map, branch_dereferences);

    for (BasicBlock* block : task.cfg()->blocks()) {
        if (!is_switch_block(block) || block->instructions().empty()) {
            continue;
        }

        auto* switch_inst = dynamic_cast<IndirectBranch*>(block->instructions().back());
        if (switch_inst == nullptr) {
            continue;
        }

        Expression* switch_expr = switch_inst->expression();
        Variable* traced = as_variable_ignoring_cast(switch_expr);
        if (traced == nullptr) {
            traced = first_variable(switch_expr);
        }
        if (traced == nullptr) {
            continue;
        }

        Variable* candidate = find_switch_candidate(traced, def_map, use_map, branch_dereferences);
        if (candidate == nullptr) {
            continue;
        }

        if (switch_expr != candidate) {
            switch_inst->substitute(switch_expr, candidate);
        }
    }
}

void RemoveGoPrologueStage::execute(DecompilerTask& task) {
    if (!task.cfg() || task.cfg()->entry_block() == nullptr) {
        return;
    }

    ControlFlowGraph* cfg = task.cfg();
    BasicBlock* root = cfg->entry_block();
    if (root->successors().size() != 2 || root->instructions().empty()) {
        return;
    }

    auto* root_branch = dynamic_cast<Branch*>(root->instructions().back());
    if (root_branch == nullptr || !branch_condition_matches_go_root_guard(root_branch)) {
        return;
    }

    Edge* succ0 = root->successors()[0];
    Edge* succ1 = root->successors()[1];
    if (succ0 == nullptr || succ1 == nullptr) {
        return;
    }

    const bool s0_morestack = successor_path_contains_morestack(succ0->target(), root);
    const bool s1_morestack = successor_path_contains_morestack(succ1->target(), root);
    if (s0_morestack == s1_morestack) {
        return;
    }

    Edge* morestack_edge = s0_morestack ? succ0 : succ1;
    cfg->remove_edge(morestack_edge);

    auto& root_insts = root->mutable_instructions();
    if (root->successors().size() == 1
        && !root_insts.empty()
        && dynamic_cast<Branch*>(root_insts.back()) != nullptr) {
        root_insts.pop_back();
    }

    remove_unreachable_blocks(cfg);
}

void RemoveStackCanaryStage::execute(DecompilerTask& task) {
    // Identify and remove stack canary setup and check sequences
}

} // namespace dewolf
