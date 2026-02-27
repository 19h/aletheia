#include "idioms.hpp"
#include "patterns.hpp"
#include <unordered_map>
#include <iostream>

namespace dewolf_idioms {

struct AnonymizationState {
    std::unordered_map<std::string, std::string> reg_map;
    std::unordered_map<std::string, std::string> const_map;
    std::unordered_map<std::string, std::string> loc_map;
    int reg_counter = 0;
    int const_counter = 0;
    int loc_counter = 0;
    
    void reset() {
        reg_map.clear();
        const_map.clear();
        loc_map.clear();
        reg_counter = 0;
        const_counter = 0;
        loc_counter = 0;
    }
};

static std::string anonymize_operand(const std::string& op, AnonymizationState& state) {
    // Very simplified anonymization mapping
    // If it looks like a register (just alpha)
    bool is_alpha = true;
    for (char c : op) {
        if (!std::isalpha(c)) { is_alpha = false; break; }
    }
    if (is_alpha) {
        if (state.reg_map.find(op) == state.reg_map.end()) {
            state.reg_map[op] = "reg_" + std::to_string(state.reg_counter++);
        }
        return state.reg_map[op];
    }
    
    // If it looks like a constant
    if (!op.empty() && (std::isdigit(op[0]) || op.find("0x") == 0)) {
        if (state.const_map.find(op) == state.const_map.end()) {
            state.const_map[op] = "const_" + std::to_string(state.const_counter++);
        }
        return state.const_map[op];
    }
    
    // Default fallback to location
    if (state.loc_map.find(op) == state.loc_map.end()) {
        state.loc_map[op] = "loc" + std::to_string(state.loc_counter++);
    }
    return state.loc_map[op];
}

std::vector<IdiomTag> IdiomMatcher::match_block(const ida::graph::BasicBlock& block) {
    std::vector<IdiomTag> tags;
    match_magic_division(block, tags);
    return tags;
}

void IdiomMatcher::match_magic_division(const ida::graph::BasicBlock& block, std::vector<IdiomTag>& tags) {
    if (block.start >= block.end) return;
    
    // Collect mnemonics and operands
    struct LocalInsn {
        ida::Address addr;
        std::string mnemonic;
        std::vector<std::string> operands;
    };
    
    std::vector<LocalInsn> local_insns;
    ida::Address current_addr = block.start;
    while (current_addr < block.end) {
        auto insn_res = ida::instruction::decode(current_addr);
        if (!insn_res) break;
        auto& insn = insn_res.value();
        
        LocalInsn li;
        li.addr = current_addr;
        li.mnemonic = insn.mnemonic();
        
        for (const auto& op : insn.operands()) {
            if (op.is_register()) {
                li.operands.push_back(op.register_name());
            } else if (op.is_immediate()) {
                li.operands.push_back("0x" + std::to_string(op.value()));
            } else {
                li.operands.push_back("loc");
            }
        }
        local_insns.push_back(li);
        
        if (insn.size() == 0) break;
        current_addr += insn.size();
    }
    
    // Now slide a window over local_insns and try to match PATTERNS
    for (size_t i = 0; i < local_insns.size(); ++i) {
        for (const auto& pat : PATTERNS) {
            if (i + pat.sequence.size() > local_insns.size()) continue;
            
            bool match = true;
            AnonymizationState state;
            
            for (size_t j = 0; j < pat.sequence.size(); ++j) {
                const auto& pat_insn = pat.sequence[j];
                const auto& real_insn = local_insns[i + j];
                
                if (pat_insn.opcode != real_insn.mnemonic) {
                    match = false;
                    break;
                }
                
                if (pat_insn.operands.size() != real_insn.operands.size()) {
                    match = false;
                    break;
                }
                
                for (size_t k = 0; k < pat_insn.operands.size(); ++k) {
                    std::string anon_op = anonymize_operand(real_insn.operands[k], state);
                    if (pat_insn.operands[k] != anon_op) {
                        match = false;
                        break;
                    }
                }
                
                if (!match) break;
            }
            
            if (match) {
                IdiomTag tag;
                tag.address = local_insns[i].addr;
                tag.tag_name = pat.type;
                tag.metadata = "Matched cluster " + std::to_string(pat.cluster);
                tags.push_back(tag);
                i += pat.sequence.size() - 1; // Skip the matched sequence
                break;
            }
        }
    }
}

} // namespace dewolf_idioms
