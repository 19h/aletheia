#pragma once
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace idiomata {

// ── Compile-friendly flat pattern representation ───────────────────────
// The pattern table has ~5000 entries spanning 37K lines. Using std::string
// and std::vector here would require thousands of constructor calls at
// static-init time and causes the compiler to spend 10+ minutes on -O1.
//
// Instead, patterns are stored as flat POD structs with const char* pointers
// into string literals (zero initialization cost). A lazy-init accessor
// converts to the rich types on first use.

/// Maximum operands per instruction in any pattern.
static constexpr std::size_t MAX_PATTERN_OPERANDS = 4;
/// Maximum instructions per pattern sequence.
static constexpr std::size_t MAX_PATTERN_SEQUENCE = 72;

struct FlatPatternInstruction {
    const char* opcode;
    const char* operands[MAX_PATTERN_OPERANDS]; // null-terminated list
};

struct FlatIdiomPattern {
    const char* type;
    int cluster;
    int sequence_len;
    FlatPatternInstruction sequence[MAX_PATTERN_SEQUENCE];
};

// Defined in patterns_data.cpp
extern const FlatIdiomPattern FLAT_PATTERNS[];
extern const std::size_t FLAT_PATTERNS_COUNT;

// ── Rich types (used by matcher code) ──────────────────────────────────
struct PatternInstruction {
    std::string opcode;
    std::vector<std::string> operands;
};

struct IdiomPattern {
    std::string type;
    int cluster;
    std::vector<PatternInstruction> sequence;
};

/// Lazily converts the flat table to rich IdiomPattern objects on first call.
const std::vector<IdiomPattern>& get_patterns();

} // namespace idiomata
