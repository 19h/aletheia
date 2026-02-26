#include "lifter.hpp"

namespace dewolf {

ida::Result<std::unique_ptr<ControlFlowGraph>> Lifter::lift_function(ida::Address function_address) {
    auto flowchart_res = ida::graph::flowchart(function_address);
    if (!flowchart_res) {
        return std::unexpected(flowchart_res.error());
    }

    auto cfg = std::make_unique<ControlFlowGraph>();
    std::unordered_map<int, BasicBlock*> block_map;

    // First pass: create all basic blocks
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

    // Second pass: fill instructions and create edges
    id = 0;
    for (const auto& ida_block : *flowchart_res) {
        BasicBlock* block = block_map[id];

        auto tags = idiom_matcher_.match_block(ida_block);
        // Process tags... (stub)

        for (ida::Address addr = ida_block.start; addr < ida_block.end; ) {
            auto insn_res = ida::instruction::decode(addr);
            if (!insn_res) {
                // If decoding fails, advance by 1 byte as a fallback
                addr += 1;
                continue;
            }
            
            Instruction* lifted_insn = lift_instruction(*insn_res);
            if (lifted_insn) {
                block->add_instruction(lifted_insn);
            }
            
            addr += insn_res->size();
        }

        // Edges (stub, requires successor resolution)
        for (int succ_id : ida_block.successors) {
            if (block_map.contains(succ_id)) {
                BasicBlock* target = block_map[succ_id];
                // For now, assume all edges are unconditional
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
    // Basic mapping from Instruction to Operation
    // This is heavily processor-dependent, simplified for now
    OperationType type = OperationType::unknown;
    
    // Example: map mnemonic or opcode ID to OperationType
    // std::string mnem = insn.mnemonic();
    // if (mnem == "add") type = OperationType::add;

    std::vector<Expression*> operands;
    return arena_.create<Operation>(type, std::move(operands), insn.size());
}

} // namespace dewolf
