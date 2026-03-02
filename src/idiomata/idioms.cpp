#include "idioms.hpp"

#include "idiom_resolver.hpp"
#include "patterns.hpp"

#include <cctype>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace idiomata {

namespace {

struct AnonymizationState {
    std::unordered_map<std::string, std::string> reg_map;
    std::unordered_map<std::string, std::string> const_map;
    std::unordered_map<std::string, std::string> loc_map;
    int reg_counter = 0;
    int const_counter = 0;
    int loc_counter = 0;

    AnonymizationState() {
        reg_map.reserve(16);
        const_map.reserve(16);
        loc_map.reserve(16);
    }

    void reset() {
        reg_map.clear();
        const_map.clear();
        loc_map.clear();
        reg_counter = 0;
        const_counter = 0;
        loc_counter = 0;
    }
};

const std::unordered_map<std::string, std::vector<const IdiomPattern*>>& patterns_by_first_opcode() {
    static const std::unordered_map<std::string, std::vector<const IdiomPattern*>> index = [] {
        std::unordered_map<std::string, std::vector<const IdiomPattern*>> by_opcode;
        by_opcode.reserve(128);
        for (const auto& pattern : get_patterns()) {
            if (pattern.sequence.empty()) {
                continue;
            }
            by_opcode[pattern.sequence.front().opcode].push_back(&pattern);
        }
        return by_opcode;
    }();
    return index;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string remove_spaces(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            out.push_back(c);
        }
    }
    return out;
}

std::string strip_memory_size_prefixes(std::string text) {
    text = remove_spaces(lower_ascii(std::move(text)));

    constexpr std::string_view prefixes[] = {
        "byteptr", "wordptr", "dwordptr", "qwordptr", "xmmwordptr", "ymmwordptr", "zmmwordptr", "ptr"
    };

    for (std::string_view prefix : prefixes) {
        size_t pos = 0;
        while ((pos = text.find(prefix, pos)) != std::string::npos) {
            text.erase(pos, prefix.size());
        }
    }

    return text;
}

bool is_numeric_token(std::string_view tok) {
    if (tok.empty()) {
        return false;
    }

    size_t i = 0;
    if (tok[0] == '-') {
        if (tok.size() == 1) {
            return false;
        }
        i = 1;
    }

    if (i + 2 <= tok.size() && tok[i] == '0' && (tok[i + 1] == 'x' || tok[i + 1] == 'X')) {
        if (i + 2 == tok.size()) {
            return false;
        }
        for (size_t j = i + 2; j < tok.size(); ++j) {
            if (!std::isxdigit(static_cast<unsigned char>(tok[j]))) {
                return false;
            }
        }
        return true;
    }

    for (size_t j = i; j < tok.size(); ++j) {
        if (!std::isdigit(static_cast<unsigned char>(tok[j]))) {
            return false;
        }
    }
    return true;
}

bool is_register_token(std::string_view tok) {
    if (tok.empty()) {
        return false;
    }

    if (tok == "sp" || tok == "bp" || tok == "ip" || tok == "pc" || tok == "lr" || tok == "fp") {
        return true;
    }

    if (tok.size() >= 2 && (tok[0] == 'r' || tok[0] == 'x' || tok[0] == 'w' || tok[0] == 'v')) {
        bool all_digits = true;
        for (size_t i = 1; i < tok.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(tok[i]))) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            return true;
        }
    }

    if (tok.size() >= 3 && (tok.starts_with("xmm") || tok.starts_with("ymm") || tok.starts_with("zmm"))) {
        for (size_t i = 3; i < tok.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(tok[i]))) {
                return false;
            }
        }
        return true;
    }

    if (tok[0] == 'r' && tok.size() >= 2) {
        bool seen_digit = false;
        for (size_t i = 1; i < tok.size(); ++i) {
            const char c = tok[i];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                seen_digit = true;
                continue;
            }
            if ((c == 'b' || c == 'w' || c == 'd') && i == tok.size() - 1 && seen_digit) {
                return true;
            }
            return false;
        }
        if (seen_digit) {
            return true;
        }
    }

    constexpr std::string_view classic[] = {
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
        "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
        "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
        "al", "ah", "bl", "bh", "cl", "ch", "dl", "dh"
    };
    for (auto reg : classic) {
        if (tok == reg) {
            return true;
        }
    }

    return false;
}

std::string intern_symbol(std::unordered_map<std::string, std::string>& table,
                          const std::string& key,
                          const char* prefix,
                          int& counter) {
    auto it = table.find(key);
    if (it != table.end()) {
        return it->second;
    }
    const std::string value = std::string(prefix) + std::to_string(counter++);
    table.emplace(key, value);
    return value;
}

std::string anonymize_operand_tokenized(const std::string& operand_text,
                                        ida::instruction::OperandType operand_type,
                                        AnonymizationState& state) {
    if (operand_type == ida::instruction::OperandType::Register) {
        return intern_symbol(state.reg_map, lower_ascii(operand_text), "reg_", state.reg_counter);
    }
    if (operand_type == ida::instruction::OperandType::Immediate) {
        return intern_symbol(state.const_map, lower_ascii(operand_text), "const_", state.const_counter);
    }

    const std::string normalized = strip_memory_size_prefixes(operand_text);
    std::string out;
    out.reserve(normalized.size());

    for (size_t i = 0; i < normalized.size();) {
        const char c = normalized[i];
        const bool token_start = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
        if (!token_start) {
            out.push_back(c);
            ++i;
            continue;
        }

        size_t j = i;
        while (j < normalized.size()) {
            const char t = normalized[j];
            if (!(std::isalnum(static_cast<unsigned char>(t)) || t == '_' || t == '-')) {
                break;
            }
            ++j;
        }

        const std::string token = normalized.substr(i, j - i);
        if (is_numeric_token(token)) {
            out += intern_symbol(state.const_map, token, "const_", state.const_counter);
        } else if (is_register_token(token)) {
            out += intern_symbol(state.reg_map, token, "reg_", state.reg_counter);
        } else {
            out += intern_symbol(state.loc_map, token, "loc", state.loc_counter);
        }
        i = j;
    }

    return out;
}

std::string render_operand_text(ida::Address ea, const ida::instruction::Operand& operand) {
    if (operand.is_register()) {
        return lower_ascii(operand.register_name());
    }

    if (operand.is_immediate()) {
        std::stringstream ss;
        ss << "0x" << std::hex << operand.value();
        return lower_ascii(ss.str());
    }

    auto text_res = ida::instruction::operand_text(ea, operand.index());
    if (text_res) {
        return lower_ascii(*text_res);
    }

    return "loc";
}

} // namespace

std::vector<IdiomTag> IdiomMatcher::match_block(const ida::graph::BasicBlock& block) {
    std::vector<IdiomTag> tags;
    match_magic_division(block, tags);
    return tags;
}

void IdiomMatcher::match_magic_division(const ida::graph::BasicBlock& block, std::vector<IdiomTag>& tags) {
    if (block.start >= block.end) {
        return;
    }

    std::vector<LocalInsn> local_insns;
    ida::Address current_addr = block.start;
    while (current_addr < block.end) {
        auto insn_res = ida::instruction::decode(current_addr);
        if (!insn_res) {
            break;
        }
        const auto& insn = *insn_res;

        LocalInsn li;
        li.addr = current_addr;
        li.mnemonic = lower_ascii(insn.mnemonic());

        for (const auto& op : insn.operands()) {
            if (op.type() == ida::instruction::OperandType::None) {
                continue;
            }
            li.operands.push_back(render_operand_text(current_addr, op));
            li.operand_types.push_back(op.type());
        }

        local_insns.push_back(std::move(li));

        if (insn.size() == 0) {
            break;
        }
        current_addr += insn.size();
    }

    AnonymizationState state;
    const auto& patterns_index = patterns_by_first_opcode();

    for (size_t i = 0; i < local_insns.size(); ++i) {
        const auto candidates_it = patterns_index.find(local_insns[i].mnemonic);
        if (candidates_it == patterns_index.end()) {
            continue;
        }

        for (const IdiomPattern* pat : candidates_it->second) {
            if (pat == nullptr) {
                continue;
            }

            if (pat->sequence.empty()) {
                continue;
            }

            if (i + pat->sequence.size() > local_insns.size()) {
                continue;
            }

            if (pat->sequence[0].operands.size() != local_insns[i].operands.size()) {
                continue;
            }

            if (pat->sequence.size() >= 2 &&
                local_insns[i + 1].mnemonic != pat->sequence[1].opcode) {
                continue;
            }

            bool operand_arity_matches = true;
            for (size_t j = 1; j < pat->sequence.size(); ++j) {
                if (pat->sequence[j].operands.size() != local_insns[i + j].operands.size()) {
                    operand_arity_matches = false;
                    break;
                }
            }
            if (!operand_arity_matches) {
                continue;
            }

            bool match = true;
            state.reset();

            for (size_t j = 0; j < pat->sequence.size() && match; ++j) {
                const auto& pat_insn = pat->sequence[j];
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
                    const auto op_type = (k < real_insn.operand_types.size())
                        ? real_insn.operand_types[k]
                        : ida::instruction::OperandType::ProcessorSpecific0;

                    const std::string anonymized = anonymize_operand_tokenized(real_insn.operands[k], op_type, state);
                    if (anonymized != pat_insn.operands[k]) {
                        match = false;
                        break;
                    }
                }
            }

            if (!match) {
                continue;
            }

            std::vector<LocalInsn> matched_seq(local_insns.begin() + static_cast<ptrdiff_t>(i),
                                               local_insns.begin() + static_cast<ptrdiff_t>(i + pat->sequence.size()));
            SequenceResolver resolver(matched_seq, state.const_map, state.reg_map);

            int64_t constant = 0;
            std::string operation;

            if (pat->type == "divs" || pat->type == "divs-long-long") {
                constant = resolver.resolve_divs();
                operation = "division";
            } else if (pat->type == "divu" || pat->type == "divu-long-long") {
                constant = resolver.resolve_divu();
                operation = "division unsigned";
            } else if (pat->type == "mods" || pat->type == "mods-long-long") {
                constant = resolver.resolve_mods();
                operation = "modulo";
            } else if (pat->type == "modu" || pat->type == "modu-long-long") {
                constant = resolver.resolve_modu();
                operation = "modulo unsigned";
            } else if (pat->type == "mul") {
                constant = resolver.resolve_mul();
                operation = "multiplication";
            }

            if (constant != 0) {
                IdiomTag tag;
                tag.address = local_insns[i].addr;
                tag.length = pat->sequence.size();
                tag.operation = std::move(operation);
                tag.operand = resolver.get_reg("reg_1");
                if (tag.operand.empty()) {
                    tag.operand = resolver.get_reg("reg_0");
                }
                tag.constant = constant;
                tags.push_back(std::move(tag));
            }

            i += pat->sequence.size() - 1;
            break;
        }
    }
}

} // namespace idiomata
