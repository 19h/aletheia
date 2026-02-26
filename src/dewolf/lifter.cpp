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

        // Lift instructions
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

        // Lift edges properly
        if (ida_block.successors.size() == 2) {
            BasicBlock* target0 = block_map.contains(ida_block.successors[0]) ? block_map[ida_block.successors[0]] : nullptr;
            BasicBlock* target1 = block_map.contains(ida_block.successors[1]) ? block_map[ida_block.successors[1]] : nullptr;
            
            if (target0) {
                Edge* e0 = arena_.create<Edge>(block, target0, EdgeType::True);
                block->add_successor(e0);
                target0->add_predecessor(e0);
            }
            if (target1) {
                Edge* e1 = arena_.create<Edge>(block, target1, EdgeType::False);
                block->add_successor(e1);
                target1->add_predecessor(e1);
            }
        } else {
            for (int succ_id : ida_block.successors) {
                if (block_map.contains(succ_id)) {
                    BasicBlock* target = block_map[succ_id];
                    EdgeType etype = EdgeType::Unconditional;
                    if (ida_block.successors.size() > 2) etype = EdgeType::Switch;
                    
                    Edge* edge = arena_.create<Edge>(block, target, etype);
                    block->add_successor(edge);
                    target->add_predecessor(edge);
                }
            }
        }
        
        id++;
    }

    return cfg;
}

std::vector<Expression*> Lifter::lift_operands(const ida::instruction::Instruction& insn) {
    std::vector<Expression*> operands;
    for (const auto& op : insn.operands()) {
        if (op.type() == ida::instruction::OperandType::None) continue;
        operands.push_back(lift_operand(op, insn.address()));
    }
    return operands;
}

Expression* Lifter::lift_operand(const ida::instruction::Operand& op, ida::Address insn_addr) {
    if (op.is_register()) {
        return arena_.create<Variable>(op.register_name(), op.byte_width());
    } else if (op.is_immediate()) {
        return arena_.create<Constant>(op.value(), op.byte_width());
    } else if (op.is_memory()) {
        auto base = arena_.create<Variable>("mem_" + std::to_string(op.value()), op.byte_width());
        return arena_.create<Operation>(OperationType::deref, std::vector<Expression*>{base}, op.byte_width());
    } else {
        // Fallback for unknown operand types: try to parse the UI text
        auto txt_res = ida::instruction::operand_text(insn_addr, op.index());
        if (txt_res) {
            std::string clean = ida::lines::tag_remove(*txt_res);
            return arena_.create<Variable>(clean, op.byte_width());
        } else {
            return arena_.create<Variable>("op_" + std::to_string(op.index()), op.byte_width());
        }
    }
}

Instruction* Lifter::lift_instruction(const ida::instruction::Instruction& insn) {
    std::string mnem = insn.mnemonic();
    
    // Normalize to lowercase for mapping
    std::string lmnem = mnem;
    for (char& c : lmnem) c = static_cast<char>(std::tolower(c));

    auto operands = lift_operands(insn);

    // --- Return instruction ---
    if (lmnem == "ret") {
        auto* ret = arena_.create<Return>(std::move(operands));
        ret->set_address(insn.address());
        return ret;
    }

    // --- Conditional branch instructions ---
    // Map conditional branch mnemonics to comparison types
    OperationType cmp_type = OperationType::unknown;
    if (lmnem == "b.le" || lmnem == "ble") cmp_type = OperationType::le;
    else if (lmnem == "b.lt" || lmnem == "blt") cmp_type = OperationType::lt;
    else if (lmnem == "b.ge" || lmnem == "bge") cmp_type = OperationType::ge;
    else if (lmnem == "b.gt" || lmnem == "bgt") cmp_type = OperationType::gt;
    else if (lmnem == "b.eq" || lmnem == "beq") cmp_type = OperationType::eq;
    else if (lmnem == "b.ne" || lmnem == "bne") cmp_type = OperationType::neq;

    if (cmp_type != OperationType::unknown) {
        // Create a Condition expression and wrap it in a Branch instruction
        Expression* lhs = operands.size() >= 1 ? operands[0] : arena_.create<Variable>("flags", 1);
        Expression* rhs = operands.size() >= 2 ? operands[1] : arena_.create<Constant>(0, lhs->size_bytes);
        auto* cond = arena_.create<Condition>(cmp_type, lhs, rhs);
        auto* branch = arena_.create<Branch>(cond);
        branch->set_address(insn.address());
        return branch;
    }

    // --- Arithmetic instructions (result in Assignment) ---
    OperationType arith_type = OperationType::unknown;
    if (lmnem == "add" || lmnem == "adds") arith_type = OperationType::add;
    else if (lmnem == "sub" || lmnem == "subs") arith_type = OperationType::sub;
    else if (lmnem == "mul") arith_type = OperationType::mul;
    else if (lmnem == "sdiv") arith_type = OperationType::div;
    else if (lmnem == "udiv") arith_type = OperationType::div_us;

    if (arith_type != OperationType::unknown) {
        if (lmnem == "cmp" && operands.size() == 2) {
            // CMP exposes a subtraction that sets flags (no destination assignment)
            auto* op = arena_.create<Operation>(OperationType::sub,
                std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
            // Wrap in a bare Assignment to a synthetic "flags" variable
            auto* flags = arena_.create<Variable>("flags", operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(flags, op);
            assign->set_address(insn.address());
            return assign;
        }

        if (operands.size() >= 3) {
            // ARM 3-operand form: ADD X0, X1, X2 -> X0 = X1 + X2
            auto* rhs = arena_.create<Operation>(arith_type,
                std::vector<Expression*>{operands[1], operands[2]}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], rhs);
            assign->set_address(insn.address());
            return assign;
        } else if (operands.size() == 2) {
            // x86 2-operand form: ADD RAX, RBX -> RAX = RAX + RBX
            auto* rhs = arena_.create<Operation>(arith_type,
                std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], rhs);
            assign->set_address(insn.address());
            return assign;
        }
    }

    // --- CMP (subtraction without assignment destination -- classified under sub) ---
    if ((lmnem == "cmp" || lmnem == "subs") && operands.size() == 2) {
        auto* op = arena_.create<Operation>(OperationType::sub,
            std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
        auto* flags = arena_.create<Variable>("flags", operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(flags, op);
        assign->set_address(insn.address());
        return assign;
    }

    // --- Data movement instructions (Assignment) ---
    bool is_mov = (lmnem == "mov");
    bool is_load = (lmnem == "ldr" || lmnem == "ldur");
    bool is_store = (lmnem == "str" || lmnem == "stur");

    if ((is_mov || is_load || is_store) && operands.size() == 2) {
        Expression* dest;
        Expression* src;
        if (is_store) {
            // Store: source is op 0, target is op 1
            dest = operands[1];
            src = operands[0];
        } else {
            // Load/Mov: target is op 0, source is op 1
            dest = operands[0];
            src = operands[1];
        }
        auto* assign = arena_.create<Assignment>(dest, src);
        assign->set_address(insn.address());
        return assign;
    }

    // --- Fallback: wrap as unknown Operation inside an Assignment ---
    // For unmapped mnemonics, create an Operation(unknown, operands) and assign
    // to a synthetic variable so it still appears in the output.
    auto* op = arena_.create<Operation>(OperationType::unknown, std::move(operands), insn.size());
    auto* mnem_var = arena_.create<Variable>(lmnem, insn.size());
    auto* assign = arena_.create<Assignment>(mnem_var, op);
    assign->set_address(insn.address());
    return assign;
}

} // namespace dewolf
