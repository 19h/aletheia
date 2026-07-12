#pragma once

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>

namespace aletheia {

inline bool is_c_quoted_literal(std::string_view text) {
    const auto quoted_at = [&](std::size_t offset) {
        if (text.size() <= offset + 1) return false;
        const char quote = text[offset];
        return (quote == '"' || quote == '\'') && text.back() == quote;
    };
    return quoted_at(0)
        || (text.starts_with("u8") && quoted_at(2))
        || ((text.starts_with('u') || text.starts_with('U')
             || text.starts_with('L')) && quoted_at(1));
}

inline bool is_c_keyword(std::string_view identifier) {
    // C11, C23 additions, alternative spellings, and common GNU C keywords
    // accepted by the portable-C gate's -std=gnu11 compiler mode.
    static constexpr std::string_view keywords[] = {
        "_Alignas", "_Alignof", "_Atomic", "_BitInt", "_Bool",
        "_Complex", "_Decimal128", "_Decimal32", "_Decimal64",
        "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
        "_Thread_local", "_Pragma", "alignas", "alignof", "asm", "auto", "bool",
        "break", "case", "char", "const", "constexpr", "continue",
        "default", "do", "double", "else", "enum", "extern", "false",
        "float", "for", "goto", "if", "inline", "int", "long",
        "nullptr", "register", "restrict", "return", "short", "signed",
        "sizeof", "static", "static_assert", "struct", "switch",
        "thread_local", "true", "typedef", "typeof", "typeof_unqual",
        "union", "unsigned", "void", "volatile", "while", "__asm",
        "__asm__", "__attribute", "__attribute__", "__auto_type",
        "__extension__", "__inline", "__inline__", "__label__",
        "__restrict", "__restrict__", "__thread", "__typeof",
        "__typeof__", "__volatile", "__volatile__",
    };
    return std::find(std::begin(keywords), std::end(keywords), identifier)
        != std::end(keywords);
}

inline bool is_valid_c_identifier(std::string_view identifier) {
    if (identifier.empty() || is_c_keyword(identifier)) return false;
    const unsigned char first = static_cast<unsigned char>(identifier.front());
    if (!(std::isalpha(first) || first == '_')) return false;
    for (unsigned char c : identifier) {
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return true;
}

inline std::string normalize_c_identifier(
    std::string_view raw,
    std::string_view fallback_prefix = "id") {
    std::string normalized;
    normalized.reserve(raw.size() + fallback_prefix.size() + 1);
    for (unsigned char c : raw) {
        normalized.push_back(
            std::isalnum(c) || c == '_' ? static_cast<char>(c) : '_');
    }

    const std::string fallback = fallback_prefix.empty()
        ? std::string("id") : std::string(fallback_prefix);
    if (normalized.empty()) normalized = fallback;
    if (std::isdigit(static_cast<unsigned char>(normalized.front()))) {
        normalized = fallback + "_" + normalized;
    }
    if (is_c_keyword(normalized)) {
        normalized = fallback + "_" + normalized;
    }
    return normalized;
}

inline std::string normalize_distinct_c_identifier(
    std::string_view raw,
    std::string_view fallback_prefix = "id") {
    (void)fallback_prefix;
    static constexpr std::string_view marker = "aletheia_encoded_";
    if (is_valid_c_identifier(raw)
        && !raw.starts_with(marker)
        && !raw.starts_with("aletheia_type_")) {
        return std::string(raw);
    }

    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded(marker);
    encoded.reserve(marker.size() + raw.size() * 2);
    for (unsigned char c : raw) {
        encoded.push_back(digits[c >> 4]);
        encoded.push_back(digits[c & 0xf]);
    }
    return encoded;
}

inline std::string encode_c_identifier_bytes(
    std::string_view marker,
    std::string_view raw) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded(marker);
    encoded.reserve(marker.size() + raw.size() * 2);
    for (unsigned char c : raw) {
        encoded.push_back(digits[c >> 4]);
        encoded.push_back(digits[c & 0xf]);
    }
    return encoded;
}

class CIdentifierAllocator {
public:
    CIdentifierAllocator() {
        static constexpr std::string_view runtime_names[] = {
            "FILE", "int8_t", "int16_t", "int32_t", "int64_t",
            "int128_t", "intptr_t", "ptrdiff_t", "size_t", "timespec",
            "uint8_t", "uint16_t", "uint32_t", "uint64_t", "uint128_t",
            "uintptr_t", "__aletheia_FILE", "__aletheia_timespec",
            "__aletheia_undefined_u64", "__aletheia_pow_u64",
            "__pcode_popcount", "__pcode_lzcount", "__pcode_float_nan",
            "__pcode_float_abs", "__pcode_float_sqrt", "__pcode_ceil",
            "__pcode_floor", "__pcode_round",
        };
        for (std::string_view name : runtime_names) {
            used_.emplace(name);
        }
    }

    void reserve(std::string name) {
        if (!name.empty()) used_.insert(std::move(name));
    }

    std::string allocate(
        std::string_view raw,
        std::string_view fallback_prefix = "id") {
        const std::string base = normalize_c_identifier(raw, fallback_prefix);
        std::string candidate = base;
        std::size_t discriminator = 2;
        while (!used_.insert(candidate).second) {
            candidate = base + "_" + std::to_string(discriminator++);
        }
        return candidate;
    }

private:
    std::unordered_set<std::string> used_;
};

} // namespace aletheia
