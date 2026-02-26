#pragma once
#include <ida/idax.hpp>
#include <ida/graph.hpp>
#include <ida/instruction.hpp>
#include "structures/cfg.hpp"
#include "../common/arena.hpp"
#include "../dewolf_idioms/idioms.hpp"
#include <memory>
#include <unordered_map>

namespace dewolf {

class Lifter {
public:
    Lifter(DecompilerArena& arena, dewolf_idioms::IdiomMatcher& idiom_matcher)
        : arena_(arena), idiom_matcher_(idiom_matcher) {}

    ida::Result<std::unique_ptr<ControlFlowGraph>> lift_function(ida::Address function_address);

private:
    DecompilerArena& arena_;
    dewolf_idioms::IdiomMatcher& idiom_matcher_;

    BasicBlock* process_block(const ida::graph::BasicBlock& ida_block, std::unordered_map<ida::Address, BasicBlock*>& block_map);
    Instruction* lift_instruction(const ida::instruction::Instruction& insn);
    Operation* map_operation(const ida::instruction::Instruction& insn);
};

} // namespace dewolf
