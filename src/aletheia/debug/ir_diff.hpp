#pragma once
#include "../pipeline/pipeline.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstddef>

namespace aletheia::debug {

struct IrSnapshot {
    struct BlockSnapshot {
        std::size_t block_id;
        std::vector<std::string> instructions;
        std::size_t content_hash = 0;  // Hash of all instruction strings
    };
    std::vector<BlockSnapshot> blocks;
};

struct IrDiff {
    struct InstructionChange {
        std::size_t block_id;
        enum Kind { Added, Removed, Modified } kind;
        std::string old_text;
        std::string new_text;
    };
    std::vector<InstructionChange> instruction_changes;
    std::vector<std::size_t> blocks_added;
    std::vector<std::size_t> blocks_removed;
    bool has_changes() const;
};

IrSnapshot capture_snapshot(const DecompilerTask& task);
IrDiff diff_snapshots(const IrSnapshot& before, const IrSnapshot& after);
std::string format_diff(const IrDiff& diff, const char* stage_name,
                         std::size_t stage_ordinal = 0,
                         const std::string& func_name = "",
                         std::uint64_t func_addr = 0);

} // namespace aletheia::debug
