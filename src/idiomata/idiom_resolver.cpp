#include "idiom_resolver.hpp"

#include "magic_maps.hpp"

#include <cctype>
#include <optional>
#include <string_view>

namespace idiomata {

namespace {

bool is_power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

int64_t parse_numeric_token(std::string_view token) {
    if (token.empty()) {
        return 0;
    }

    bool negative = false;
    if (token.front() == '-') {
        negative = true;
        token.remove_prefix(1);
    }

    int base = 10;
    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        base = 16;
        token.remove_prefix(2);
    }

    if (token.empty()) {
        return 0;
    }

    std::string owned(token);
    uint64_t raw = 0;
    try {
        raw = std::stoull(owned, nullptr, base);
    } catch (...) {
        return 0;
    }

    int64_t signed_value = static_cast<int64_t>(raw);
    return negative ? -signed_value : signed_value;
}

bool is_sign_fixup_pattern(const LocalInsn& insn, const SequenceResolver& resolver, std::string& sign_reg) {
    if ((insn.mnemonic != "sar" && insn.mnemonic != "shr") || insn.operands.size() < 2) {
        return false;
    }
    if (!insn.operands[1].starts_with("const")) {
        return false;
    }
    if (resolver.get_const_val(insn.operands[1]) != 31) {
        return false;
    }
    sign_reg = insn.operands[0];
    return true;
}

} // namespace

int64_t SequenceResolver::get_const_val(const std::string& anon_name) const {
    return parse_numeric_token(get_const(anon_name));
}

uint64_t SequenceResolver::accumulate_shr_amount() const {
    uint64_t power = 0;
    for (const auto& insn : sequence_) {
        if (insn.mnemonic != "shr") {
            continue;
        }
        for (const auto& op : insn.operands) {
            if (!op.starts_with("const")) {
                continue;
            }
            power += static_cast<uint64_t>(get_const_val(op));
            break;
        }
    }
    return power;
}

std::pair<uint64_t, int> SequenceResolver::get_mul_constant_and_position() const {
    for (size_t i = 0; i < sequence_.size(); ++i) {
        const auto& insn = sequence_[i];
        if (insn.mnemonic != "imul" && insn.mnemonic != "mul") {
            continue;
        }
        for (const auto& op : insn.operands) {
            if (op.starts_with("const")) {
                return {static_cast<uint64_t>(get_const_val(op)), static_cast<int>(i)};
            }
        }
        return {0, static_cast<int>(i)};
    }
    return {0, -1};
}

uint64_t SequenceResolver::backtrack_magic_number(int mul_pos) const {
    if (mul_pos < 0 || static_cast<size_t>(mul_pos) >= sequence_.size()) {
        return 0;
    }

    const auto& mul_insn = sequence_[static_cast<size_t>(mul_pos)];
    const std::string lhs = mul_insn.operands.empty() ? "" : mul_insn.operands.front();
    const std::string rhs = mul_insn.operands.empty() ? "" : mul_insn.operands.back();

    const std::string lhs_reg = get_reg(lhs);
    const std::string rhs_reg = get_reg(rhs);

    for (int i = mul_pos; i >= 0; --i) {
        const auto& current = sequence_[static_cast<size_t>(i)];
        if (current.mnemonic != "mov" || current.operands.size() < 2) {
            continue;
        }

        const std::string dest_reg = get_reg(current.operands[0]);
        const bool touches_mul_input = (dest_reg == lhs_reg || dest_reg == rhs_reg);
        if (!touches_mul_input) {
            continue;
        }

        const std::string& src = current.operands.back();
        if (src.starts_with("const")) {
            return static_cast<uint64_t>(get_const_val(src));
        }
    }

    return 0;
}

int64_t SequenceResolver::lookup_signed_magic(uint64_t magic, uint64_t power) const {
    if (auto q = lookup_magic_array(magic, power, MAGIC_MAP)) {
        return *q;
    }
    if (auto q = lookup_magic_array(magic, power, MAGIC_MAP_LONG)) {
        return *q;
    }
    return 0;
}

int64_t SequenceResolver::lookup_unsigned_magic(uint64_t magic, uint64_t power) const {
    if (auto q = lookup_magic_array(magic, power, UNSIGNED_MAGIC_MAP)) {
        return *q;
    }
    return 0;
}

int64_t SequenceResolver::normalize_divisor_sign(int64_t value) {
    if (value < 0) {
        return -value;
    }
    return value;
}

int64_t SequenceResolver::resolve_divs() const {
    auto [magic, mul_pos] = get_mul_constant_and_position();
    if (magic == 0) {
        magic = backtrack_magic_number(mul_pos);
    }
    if (magic == 0) {
        return 0;
    }

    uint64_t power = 0;
    int power_insn_index = -1;
    uint64_t extra = 0;

    for (size_t i = 0; i < sequence_.size(); ++i) {
        const auto& insn = sequence_[i];
        if (insn.mnemonic != "sar" && insn.mnemonic != "shr") {
            continue;
        }

        for (const auto& op : insn.operands) {
            if (!op.starts_with("const")) {
                continue;
            }
            const auto val = static_cast<uint64_t>(get_const_val(op));
            if (val >= 32 && power == 0) {
                power = val;
                power_insn_index = static_cast<int>(i);
            }
            break;
        }
    }

    for (size_t i = 0; i < sequence_.size(); ++i) {
        if (static_cast<int>(i) == power_insn_index) {
            continue;
        }
        const auto& insn = sequence_[i];
        if (insn.mnemonic != "sar" && insn.mnemonic != "shr") {
            continue;
        }

        for (const auto& op : insn.operands) {
            if (!op.starts_with("const")) {
                continue;
            }
            const auto val = static_cast<uint64_t>(get_const_val(op));
            if (val < 0x1F && extra == 0) {
                extra = val;
            }
            break;
        }
    }

    power += extra;
    if (power_insn_index < 0) {
        power += 32;
    }

    const uint64_t normalized_magic = magic & 0xFFFFFFFFULL;
    int64_t quotient = lookup_signed_magic(normalized_magic, power);
    if (quotient == 0) {
        return 0;
    }

    bool should_flip_sign = false;
    std::string sign_reg;
    for (const auto& insn : sequence_) {
        is_sign_fixup_pattern(insn, *this, sign_reg);
        if (insn.mnemonic == "sub" && !sign_reg.empty() && insn.operands.size() >= 2) {
            if (insn.operands[1] == sign_reg) {
                should_flip_sign = true;
            }
        }
    }

    quotient = normalize_divisor_sign(quotient);
    if (should_flip_sign) {
        quotient = -quotient;
    }
    return quotient;
}

int64_t SequenceResolver::resolve_divu() const {
    uint64_t power = accumulate_shr_amount();
    auto [magic, mul_pos] = get_mul_constant_and_position();
    if (magic == 0) {
        magic = backtrack_magic_number(mul_pos);
    }
    if (magic == 0) {
        return 0;
    }

    const uint64_t normalized_magic = magic & 0xFFFFFFFFULL;
    bool starts_with_mul = !sequence_.empty()
        && (sequence_.front().mnemonic == "mul" || sequence_.front().mnemonic == "imul");

    int64_t result = 0;
    if (starts_with_mul) {
        result = lookup_unsigned_magic(normalized_magic, power);
        if (result == 0) {
            result = lookup_unsigned_magic(normalized_magic, power + 32);
        }
    }

    if (result == 0) {
        result = lookup_signed_magic(normalized_magic, power);
    }
    if (result == 0) {
        result = lookup_unsigned_magic(normalized_magic, power + 3);
    }
    if (result == 0) {
        for (int i = 0; i < 50 && result == 0; ++i) {
            result = lookup_unsigned_magic(normalized_magic - static_cast<uint64_t>(i), power + 3);
        }
    }

    if (result == 0) {
        uint64_t shift = 0;
        for (const auto& insn : sequence_) {
            if (insn.mnemonic != "shr") {
                continue;
            }
            for (const auto& op : insn.operands) {
                if (op.starts_with("const")) {
                    shift = static_cast<uint64_t>(get_const_val(op));
                    break;
                }
            }
            break;
        }

        power += shift;
        if (shift < 32) {
            for (int i = 0; i < 50 && result == 0; ++i) {
                const uint64_t candidate = normalized_magic * (1ULL << shift) - static_cast<uint64_t>(i);
                result = lookup_signed_magic(candidate, power);
            }
        }
    }

    return normalize_divisor_sign(result);
}

int64_t SequenceResolver::resolve_mods() const {
    for (const auto& insn : sequence_) {
        if (insn.mnemonic != "and" || insn.operands.size() < 2) {
            continue;
        }
        const std::string& mask_op = insn.operands[1];
        if (!mask_op.starts_with("const")) {
            continue;
        }
        const uint64_t mask = static_cast<uint64_t>(get_const_val(mask_op));
        const uint64_t divisor = mask + 1;
        if (is_power_of_two(divisor)) {
            return static_cast<int64_t>(divisor);
        }
    }

    return resolve_divs();
}

int64_t SequenceResolver::resolve_modu() const {
    for (const auto& insn : sequence_) {
        if (insn.mnemonic != "and" || insn.operands.size() < 2) {
            continue;
        }
        const std::string& mask_op = insn.operands[1];
        if (!mask_op.starts_with("const")) {
            continue;
        }
        const uint64_t mask = static_cast<uint64_t>(get_const_val(mask_op));
        const uint64_t divisor = mask + 1;
        if (is_power_of_two(divisor)) {
            return static_cast<int64_t>(divisor);
        }
    }

    return resolve_divu();
}

int64_t SequenceResolver::resolve_mul() const {
    for (const auto& insn : sequence_) {
        if (insn.mnemonic == "imul") {
            for (const auto& op : insn.operands) {
                if (op.starts_with("const")) {
                    return normalize_divisor_sign(get_const_val(op));
                }
            }
        }

        if (insn.mnemonic == "shl" && insn.operands.size() >= 2 && insn.operands[1].starts_with("const")) {
            const auto shift = static_cast<uint64_t>(get_const_val(insn.operands[1]));
            if (shift < 63) {
                return static_cast<int64_t>(1ULL << shift);
            }
        }
    }

    return 0;
}

} // namespace idiomata
