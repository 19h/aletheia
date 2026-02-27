#include "instruction_length_handler.hpp"

#include <algorithm>
#include <vector>

namespace dewolf {

namespace {

class Handler {
public:
    Handler(DecompilerArena& arena, InstructionLengthBounds bounds)
        : arena_(arena), bounds_(bounds) {}

    void run(AbstractSyntaxForest* forest) {
        if (!forest || !forest->root()) {
            return;
        }
        visit_node(forest->root());
    }

private:
    static std::size_t expression_complexity(Expression* expr) {
        if (!expr) return 0;
        if (dynamic_cast<Constant*>(expr) != nullptr || dynamic_cast<Variable*>(expr) != nullptr) {
            return 1;
        }
        if (auto* op = dynamic_cast<Operation*>(expr)) {
            std::size_t total = 0;
            for (Expression* child : op->operands()) {
                total += expression_complexity(child);
            }
            return total;
        }
        if (auto* list = dynamic_cast<ListOperation*>(expr)) {
            std::size_t total = 0;
            for (Expression* child : list->operands()) {
                total += expression_complexity(child);
            }
            return total;
        }
        return 1;
    }

    static bool is_simple(Expression* expr) {
        return dynamic_cast<Constant*>(expr) != nullptr || dynamic_cast<Variable*>(expr) != nullptr;
    }

    Variable* make_tmp(Expression* source) {
        auto* tmp = arena_.create<Variable>("tmp_" + std::to_string(tmp_index_++), source ? source->size_bytes : 0);
        if (source) {
            tmp->set_ir_type(source->ir_type());
            tmp->set_ssa_version(0);
        }
        return tmp;
    }

    void substitute_with_tmp(Instruction* owner, Expression* replacee, std::vector<Instruction*>& out) {
        if (!owner || !replacee) return;

        auto* tmp = make_tmp(replacee);
        auto* tmp_assign = arena_.create<Assignment>(tmp, replacee);
        simplify_assignment(tmp_assign, out);
        out.push_back(tmp_assign);
        owner->substitute(replacee, tmp);
    }

    void simplify_binary_target(
        Instruction* owner,
        Operation* op,
        std::size_t target_bound,
        std::vector<Instruction*>& out) {
        if (!owner || !op || op->operands().size() != 2) return;

        Expression* lhs = op->operands()[0];
        Expression* rhs = op->operands()[1];

        if (target_bound == 1) {
            substitute_with_tmp(owner, op, out);
            return;
        }

        if (expression_complexity(lhs) >= expression_complexity(rhs) && !is_simple(lhs)) {
            substitute_with_tmp(owner, lhs, out);
        }

        if (expression_complexity(op) > target_bound && !is_simple(rhs)) {
            substitute_with_tmp(owner, rhs, out);
        }
    }

    void simplify_operand_list(
        Instruction* owner,
        std::vector<Expression*>& operands,
        std::size_t target_bound,
        std::vector<Instruction*>& out) {
        if (!owner) return;

        if (operands.size() == 1) {
            Expression* only = operands[0];
            if (!only) return;
            if (target_bound == 1) {
                substitute_with_tmp(owner, only, out);
                return;
            }
            simplify_target(owner, only, target_bound, out);
            return;
        }

        for (Expression* operand : operands) {
            if (!operand || is_simple(operand)) continue;
            substitute_with_tmp(owner, operand, out);

            std::size_t current = 0;
            for (Expression* updated : operands) {
                current += expression_complexity(updated);
            }
            if (current <= target_bound) {
                return;
            }
        }
    }

    void simplify_target(
        Instruction* owner,
        Expression* target,
        std::size_t target_bound,
        std::vector<Instruction*>& out) {
        if (!owner || !target) return;
        if (expression_complexity(target) <= target_bound) return;

        if (auto* call = dynamic_cast<Call*>(target)) {
            auto& mutable_ops = call->mutable_operands();
            simplify_operand_list(owner, mutable_ops, target_bound, out);
            return;
        }

        if (auto* list = dynamic_cast<ListOperation*>(target)) {
            auto& mutable_ops = list->mutable_operands();
            simplify_operand_list(owner, mutable_ops, target_bound, out);
            return;
        }

        if (auto* op = dynamic_cast<Operation*>(target)) {
            if (op->operands().size() == 2) {
                simplify_binary_target(owner, op, target_bound, out);
                return;
            }
            if (op->operands().size() == 1) {
                simplify_target(owner, op->operands()[0], target_bound, out);
            }
        }
    }

    void simplify_assignment(Assignment* assign, std::vector<Instruction*>& out) {
        if (!assign || !assign->value()) return;
        const std::size_t bound = dynamic_cast<Call*>(assign->value()) != nullptr
            ? bounds_.call_operation
            : bounds_.assignment_instr;
        simplify_target(assign, assign->value(), bound, out);
    }

    void simplify_return(Return* ret, std::vector<Instruction*>& out) {
        if (!ret) return;
        auto& values = ret->mutable_values();

        std::size_t total = 0;
        for (Expression* value : values) {
            total += expression_complexity(value);
        }
        if (total <= bounds_.return_instr) {
            return;
        }

        simplify_operand_list(ret, values, bounds_.return_instr, out);
    }

    void simplify_instruction(Instruction* inst, std::vector<Instruction*>& out) {
        if (auto* assign = dynamic_cast<Assignment*>(inst)) {
            simplify_assignment(assign, out);
            return;
        }
        if (auto* ret = dynamic_cast<Return*>(inst)) {
            simplify_return(ret, out);
        }
    }

    void process_instruction_slot(std::vector<Instruction*>& instructions, std::size_t index) {
        if (index >= instructions.size()) return;
        std::vector<Instruction*> generated;
        simplify_instruction(instructions[index], generated);
        if (generated.empty()) return;
        instructions.insert(instructions.begin() + static_cast<std::ptrdiff_t>(index), generated.begin(), generated.end());
    }

    void process_block(BasicBlock* block) {
        if (!block) return;
        auto& instructions = block->mutable_instructions();
        for (std::size_t i = 0; i < instructions.size(); ++i) {
            const std::size_t before = instructions.size();
            process_instruction_slot(instructions, i);
            const std::size_t after = instructions.size();
            if (after > before) {
                i += (after - before);
            }
        }
    }

    void visit_node(AstNode* node) {
        if (!node) return;

        if (auto* code = dynamic_cast<CodeNode*>(node)) {
            process_block(code->block());
            return;
        }
        if (auto* seq = dynamic_cast<SeqNode*>(node)) {
            for (AstNode* child : seq->nodes()) {
                visit_node(child);
            }
            return;
        }
        if (auto* if_node = dynamic_cast<IfNode*>(node)) {
            visit_node(if_node->true_branch());
            visit_node(if_node->false_branch());
            return;
        }
        if (auto* for_loop = dynamic_cast<ForLoopNode*>(node)) {
            std::vector<Instruction*> sink;
            simplify_instruction(for_loop->declaration(), sink);
            simplify_instruction(for_loop->modification(), sink);
            visit_node(for_loop->body());
            return;
        }
        if (auto* loop = dynamic_cast<LoopNode*>(node)) {
            visit_node(loop->body());
            return;
        }
        if (auto* sw = dynamic_cast<SwitchNode*>(node)) {
            for (CaseNode* case_node : sw->cases()) {
                visit_node(case_node);
            }
            return;
        }
        if (auto* case_node = dynamic_cast<CaseNode*>(node)) {
            visit_node(case_node->body());
        }
    }

    DecompilerArena& arena_;
    InstructionLengthBounds bounds_;
    std::size_t tmp_index_ = 0;
};

} // namespace

void InstructionLengthHandler::apply(
    AbstractSyntaxForest* forest,
    DecompilerArena& arena,
    InstructionLengthBounds bounds) {
    Handler handler(arena, bounds);
    handler.run(forest);
}

} // namespace dewolf
