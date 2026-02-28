#pragma once
#include <ida/idax.hpp>
#include <ida/graph.hpp>
#include <ida/instruction.hpp>
#include "structures/cfg.hpp"
#include "../common/arena.hpp"
#include "../idiomata/idioms.hpp"
#include <memory>
#include <unordered_map>

namespace aletheia {

class Lifter {
public:
    Lifter(DecompilerArena& arena, idiomata::IdiomMatcher& idiom_matcher)
        : arena_(arena), idiom_matcher_(idiom_matcher) {}

    ida::Result<std::unique_ptr<ControlFlowGraph>> lift_function(
        ida::Address function_address,
        std::vector<idiomata::IdiomTag>* idiom_tags_out = nullptr);
    void populate_task_signature(class DecompilerTask& task);

private:
    DecompilerArena& arena_;
    idiomata::IdiomMatcher& idiom_matcher_;

    BasicBlock* process_block(const ida::graph::BasicBlock& ida_block, std::unordered_map<ida::Address, BasicBlock*>& block_map);
    Instruction* lift_instruction(const ida::instruction::Instruction& insn);
    
    // Operand lifting helpers
    std::vector<Expression*> lift_operands(const ida::instruction::Instruction& insn);
    Expression* lift_operand(const ida::instruction::Operand& op, ida::Address insn_addr);

    // IR construction helpers
    Assignment* make_binary_assign(OperationType op_type, std::vector<Expression*>& operands, ida::Address addr);
    Assignment* make_unary_assign(OperationType op_type, std::vector<Expression*>& operands, ida::Address addr);
};

} // namespace aletheia
