#pragma once
#include <ida/idax.hpp>
#include <ida/graph.hpp>
#include <ida/instruction.hpp>
#include <ida/function.hpp>
#include "structures/cfg.hpp"
#include "../common/arena.hpp"
#include "../idiomata/idioms.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace aletheia {

/// Cached frame layout information for stack variable recovery during lifting.
struct FrameLayout {
    /// Map from frame-base-relative byte offset to FrameVariable info.
    /// Frame base is typically the saved frame pointer (RBP/X29).
    std::unordered_map<std::int64_t, ida::function::FrameVariable> offset_to_var;

    /// Sizes of each frame area.
    std::size_t local_size = 0;
    std::size_t regs_size = 0;
    std::size_t args_size = 0;
    std::size_t total_size = 0;

    bool valid = false;
};

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

    /// Per-function frame layout, populated at the start of lift_function().
    FrameLayout frame_layout_;

    /// Current function address (for SP delta queries).
    ida::Address current_function_ea_ = ida::BadAddress;

    /// Parameter register map (populated during populate_task_signature).
    /// Maps lowercase register name -> parameter index.
    std::unordered_map<std::string, int> param_register_map_;

    BasicBlock* process_block(const ida::graph::BasicBlock& ida_block, std::unordered_map<ida::Address, BasicBlock*>& block_map);
    Instruction* lift_instruction(const ida::instruction::Instruction& insn);
    
    // Operand lifting helpers
    std::vector<Expression*> lift_operands(const ida::instruction::Instruction& insn);
    Expression* lift_operand(const ida::instruction::Operand& op, ida::Address insn_addr);

    // IR construction helpers
    Assignment* make_binary_assign(OperationType op_type, std::vector<Expression*>& operands, ida::Address addr);
    Assignment* make_unary_assign(OperationType op_type, std::vector<Expression*>& operands, ida::Address addr);

    /// Query IDA frame and populate frame_layout_.
    void populate_frame_layout(ida::Address function_address);

    /// Try to resolve a frame-base-relative offset to a named variable using IDA's frame info.
    std::optional<std::string> resolve_frame_variable(std::int64_t frame_offset, std::size_t access_size) const;

    /// Tag a Variable with stack/parameter metadata based on its name and context.
    void tag_variable(Variable* var, ida::Address insn_addr) const;
};

} // namespace aletheia
