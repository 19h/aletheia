#include "../src/idiomata/idiom_resolver.hpp"
#include <cassert>
#include <iostream>

using namespace idiomata;

int main() {
    // 32-bit division by 3: magic = 1431655766 (0x55555556), extra shift = 0.
    std::vector<LocalInsn> seq1 = {
        {0, "imul", {"reg_0", "reg_0", "const_1"}},
        {4, "sar", {"reg_0", "const_2"}}
    };
    std::unordered_map<std::string, std::string> const_map1 = {
        {"0x55555556", "const_1"},
        {"0x0", "const_2"}
    };
    std::unordered_map<std::string, std::string> reg_map1 = { {"eax", "reg_0"} };
    assert(SequenceResolver(seq1, const_map1, reg_map1).resolve_divs() == 3);

    // 32-bit signed division by 5: magic = -1717986919 (0x99999999), extra shift = 1.
    std::vector<LocalInsn> seq2 = {
        {0, "imul", {"reg_0", "reg_0", "const_1"}},
        {4, "sar", {"reg_0", "const_2"}},
        {8, "sar", {"reg_0", "const_3"}}
    };
    std::unordered_map<std::string, std::string> const_map2 = {
        {"0x99999999", "const_1"},
        {"0x20", "const_2"}, // 32
        {"0x1", "const_3"}   // 1 (extra shift)
    };
    std::unordered_map<std::string, std::string> reg_map2 = { {"eax", "reg_0"} };
    assert(SequenceResolver(seq2, const_map2, reg_map2).resolve_divs() == 5);
    
    // Unsigned division by 5: magic = 3435973837 (0xCCCCCCCD), shift = 2
    std::vector<LocalInsn> seq3 = {
        {0, "mul", {"reg_0", "reg_0", "const_1"}},
        {4, "shr", {"reg_0", "const_2"}}
    };
    std::unordered_map<std::string, std::string> const_map3 = {
        {"0xcccccccd", "const_1"},
        {"0x2", "const_2"} // power=2
    };
    std::unordered_map<std::string, std::string> reg_map3 = { {"eax", "reg_0"} };
    assert(SequenceResolver(seq3, const_map3, reg_map3).resolve_divu() == 5);

    // Unsigned modulo by power-of-two via mask: x & 0xF -> % 16
    std::vector<LocalInsn> seq4 = {
        {0, "and", {"reg_0", "const_0"}}
    };
    std::unordered_map<std::string, std::string> const_map4 = {
        {"0xf", "const_0"}
    };
    std::unordered_map<std::string, std::string> reg_map4 = { {"eax", "reg_0"} };
    assert(SequenceResolver(seq4, const_map4, reg_map4).resolve_modu() == 16);

    // Multiplication by left shift amount.
    std::vector<LocalInsn> seq5 = {
        {0, "shl", {"reg_0", "const_0"}}
    };
    std::unordered_map<std::string, std::string> const_map5 = {
        {"0x3", "const_0"}
    };
    std::unordered_map<std::string, std::string> reg_map5 = { {"eax", "reg_0"} };
    assert(SequenceResolver(seq5, const_map5, reg_map5).resolve_mul() == 8);

    std::cout << "All idiom semantic resolution tests passed!" << std::endl;
    return 0;
}
