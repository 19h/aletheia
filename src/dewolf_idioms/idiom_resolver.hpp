#pragma once
#include "idioms.hpp"
#include <ida/instruction.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>

namespace dewolf_idioms {

struct LocalInsn {
    ida::Address addr;
    std::string mnemonic;
    std::vector<std::string> operands;
    std::vector<ida::instruction::OperandType> operand_types;
};

class SequenceResolver {
public:
    SequenceResolver(const std::vector<LocalInsn>& seq, 
                     const std::unordered_map<std::string, std::string>& const_map,
                     const std::unordered_map<std::string, std::string>& reg_map)
        : sequence_(seq) {
        for (const auto& [k, v] : const_map) {
            reverse_const_map_[v] = k;
        }
        for (const auto& [k, v] : reg_map) {
            reverse_reg_map_[v] = k;
        }
    }

    int64_t resolve_divs() const;
    int64_t resolve_divu() const;
    int64_t resolve_mods() const;
    int64_t resolve_modu() const;
    int64_t resolve_mul() const;
    
    // Fallbacks
    std::string get_const(const std::string& anon_name) const {
        auto it = reverse_const_map_.find(anon_name);
        return it != reverse_const_map_.end() ? it->second : "";
    }
    
    int64_t get_const_val(const std::string& anon_name) const;
    
    std::string get_reg(const std::string& anon_name) const {
        auto it = reverse_reg_map_.find(anon_name);
        return it != reverse_reg_map_.end() ? it->second : "";
    }

private:
    const std::vector<LocalInsn>& sequence_;
    std::unordered_map<std::string, std::string> reverse_const_map_;
    std::unordered_map<std::string, std::string> reverse_reg_map_;

    uint64_t accumulate_shr_amount() const;
    std::pair<uint64_t, int> get_mul_constant_and_position() const;
    uint64_t backtrack_magic_number(int mul_pos) const;
    int64_t lookup_signed_magic(uint64_t magic, uint64_t power) const;
    int64_t lookup_unsigned_magic(uint64_t magic, uint64_t power) const;
    static int64_t normalize_divisor_sign(int64_t value);
};

} // namespace dewolf_idioms
