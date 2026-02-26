#include "lifter.hpp"
#include <ida/lines.hpp>

namespace dewolf {

ida::Result<std::unique_ptr<ControlFlowGraph>> Lifter::lift_function(ida::Address function_address) {
    auto flowchart_res = ida::graph::flowchart(function_address);
    if (!flowchart_res) {
        return std::unexpected(flowchart_res.error());
    }

    auto cfg = std::make_unique<ControlFlowGraph>();
    std::unordered_map<int, BasicBlock*> block_map;

    int id = 0;
    for (const auto& ida_block : *flowchart_res) {
        BasicBlock* block = arena_.create<BasicBlock>(id);
        block_map[id] = block;
        cfg->add_block(block);
        
        if (id == 0) {
            cfg->set_entry_block(block);
        }
        id++;
    }

    id = 0;
    for (const auto& ida_block : *flowchart_res) {
        BasicBlock* block = block_map[id];

        for (ida::Address addr = ida_block.start; addr < ida_block.end; ) {
            auto insn_res = ida::instruction::decode(addr);
            if (!insn_res) {
                addr += 1;
                continue;
            }
            
            Instruction* lifted_insn = lift_instruction(*insn_res);
            if (lifted_insn) {
                block->add_instruction(lifted_insn);
            }
            
            addr += insn_res->size();
        }

        for (int succ_id : ida_block.successors) {
            if (block_map.contains(succ_id)) {
                BasicBlock* target = block_map[succ_id];
                Edge* edge = arena_.create<Edge>(block, target, EdgeType::Unconditional);
                block->add_successor(edge);
                target->add_predecessor(edge);
            }
        }
        
        id++;
    }

    return cfg;
}

Instruction* Lifter::lift_instruction(const ida::instruction::Instruction& insn) {
    Operation* op = map_operation(insn);
    if (!op) return nullptr;
    return arena_.create<Instruction>(insn.address(), op);
}

Operation* Lifter::map_operation(const ida::instruction::Instruction& insn) {
    std::string mnem = insn.mnemonic();
    
    // Normalize to lowercase for mapping
    std::string lmnem = mnem;
    for (char& c : lmnem) c = std::tolower(c);

    OperationType type = OperationType::unknown;

    if (lmnem == "add" || lmnem == "adds") type = OperationType::add;
    else if (lmnem == "sub" || lmnem == "subs") type = OperationType::sub;
    else if (lmnem == "mul") type = OperationType::mul;
    else if (lmnem == "sdiv" || lmnem == "udiv") type = OperationType::div;
    else if (lmnem == "mov") type = OperationType::assign;
    else if (lmnem == "str" || lmnem == "stur") type = OperationType::assign;
    else if (lmnem == "ldr" || lmnem == "ldur") type = OperationType::assign;
    else if (lmnem == "cmp") type = OperationType::sub; // Simplified modeling
    else if (lmnem == "ret") {
        type = OperationType::call; // Model ret as a call for now
    }
    
    std::vector<Expression*> operands;
    for (const auto& op : insn.operands()) {
        if (op.type() == ida::instruction::OperandType::None) continue;

        if (op.is_register()) {
            operands.push_back(arena_.create<Variable>(op.register_name(), op.byte_width()));
        } else if (op.is_immediate()) {
            operands.push_back(arena_.create<Constant>(op.value(), op.byte_width()));
        } else if (op.is_memory()) {
            auto base = arena_.create<Variable>("mem_" + std::to_string(op.value()), op.byte_width());
            operands.push_back(arena_.create<Operation>(OperationType::deref, std::vector<Expression*>{base}, op.byte_width()));
        } else {
            // Fallback for unknown operand types: try to parse the UI text
            auto txt_res = ida::instruction::operand_text(insn.address(), op.index());
            if (txt_res) {
                std::string clean = ida::lines::tag_remove(*txt_res);
                operands.push_back(arena_.create<Variable>(clean, op.byte_width()));
            } else {
                operands.push_back(arena_.create<Variable>("op_" + std::to_string(op.index()), op.byte_width()));
            }
        }
    }

    // Structure assignments correctly: Left-Hand Side vs Right-Hand Side
    if (type == OperationType::add || type == OperationType::sub || type == OperationType::mul || type == OperationType::div) {
        if (operands.size() >= 3) {
            auto rhs = arena_.create<Operation>(type, std::vector<Expression*>{operands[1], operands[2]}, operands[0]->size_bytes);
            return arena_.create<Operation>(OperationType::assign, std::vector<Expression*>{operands[0], rhs}, operands[0]->size_bytes);
        } else if (operands.size() == 2) {
            // e.g. x86 style ADD RAX, RBX -> RAX = RAX + RBX
            auto rhs = arena_.create<Operation>(type, std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
            return arena_.create<Operation>(OperationType::assign, std::vector<Expression*>{operands[0], rhs}, operands[0]->size_bytes);
        }
    }

    if (type == OperationType::assign && operands.size() == 2) {
        if (lmnem == "str" || lmnem == "stur") {
            // Store: Source is op 0, Target is op 1
            return arena_.create<Operation>(OperationType::assign, std::vector<Expression*>{operands[1], operands[0]}, operands[0]->size_bytes);
        } else {
            // Load/Mov: Target is op 0, Source is op 1
            return arena_.create<Operation>(OperationType::assign, std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
        }
    }

    if (type == OperationType::unknown || (type == OperationType::call && lmnem == "ret")) {
        auto mnem_var = arena_.create<Variable>(lmnem, insn.size());
        operands.insert(operands.begin(), mnem_var);
        return arena_.create<Operation>(OperationType::call, std::move(operands), insn.size());
    }

    return arena_.create<Operation>(type, std::move(operands), insn.size());
}

} // namespace dewolf
