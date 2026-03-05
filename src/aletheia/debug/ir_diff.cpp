#include "ir_diff.hpp"
#include "ir_serializer.hpp"
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <format>
#include <functional>

namespace aletheia::debug {

bool IrDiff::has_changes() const {
    return !instruction_changes.empty() || !blocks_added.empty() || !blocks_removed.empty();
}

static std::size_t hash_instructions(const std::vector<std::string>& instructions) {
    std::size_t h = 0;
    for (const auto& s : instructions) {
        h ^= std::hash<std::string>{}(s) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
}

IrSnapshot capture_snapshot(const DecompilerTask& task) {
    IrSnapshot snap;
    auto* cfg = task.cfg();
    if (!cfg) return snap;

    for (auto* block : cfg->blocks()) {
        IrSnapshot::BlockSnapshot bs;
        bs.block_id = block->id();
        for (auto* inst : block->instructions()) {
            bs.instructions.push_back(ir_to_string(inst));
        }
        bs.content_hash = hash_instructions(bs.instructions);
        snap.blocks.push_back(std::move(bs));
    }
    return snap;
}

IrDiff diff_snapshots(const IrSnapshot& before, const IrSnapshot& after) {
    IrDiff diff;

    // Build maps keyed by block_id
    std::unordered_map<std::size_t, const IrSnapshot::BlockSnapshot*> before_map;
    std::unordered_map<std::size_t, const IrSnapshot::BlockSnapshot*> after_map;

    for (auto& b : before.blocks) before_map[b.block_id] = &b;
    for (auto& a : after.blocks) after_map[a.block_id] = &a;

    // Detect block additions and removals
    for (auto& [id, _] : before_map) {
        if (!after_map.contains(id)) {
            diff.blocks_removed.push_back(id);
        }
    }
    for (auto& [id, _] : after_map) {
        if (!before_map.contains(id)) {
            diff.blocks_added.push_back(id);
        }
    }

    // For removed blocks, report all instructions as removed
    for (auto id : diff.blocks_removed) {
        auto* bs = before_map[id];
        for (const auto& inst : bs->instructions) {
            diff.instruction_changes.push_back({id, IrDiff::InstructionChange::Removed, inst, ""});
        }
    }

    // For added blocks, report all instructions as added
    for (auto id : diff.blocks_added) {
        auto* bs = after_map[id];
        for (const auto& inst : bs->instructions) {
            diff.instruction_changes.push_back({id, IrDiff::InstructionChange::Added, "", inst});
        }
    }

    // For matched blocks, compare instructions using hash-based matching
    for (auto& [id, before_bs] : before_map) {
        auto it = after_map.find(id);
        if (it == after_map.end()) continue; // Block removed, already handled
        auto* after_bs = it->second;

        // Hash-based fast path: if block content identical, skip
        if (before_bs->content_hash == after_bs->content_hash &&
            before_bs->instructions.size() == after_bs->instructions.size()) {
            continue; // No changes in this block
        }

        // Hash-based instruction matching
        std::unordered_multiset<std::string> before_set(before_bs->instructions.begin(),
                                                         before_bs->instructions.end());
        std::unordered_multiset<std::string> after_set(after_bs->instructions.begin(),
                                                        after_bs->instructions.end());

        // Instructions removed: in before but not after
        auto before_copy = before_set;
        for (const auto& s : after_set) {
            auto it2 = before_copy.find(s);
            if (it2 != before_copy.end()) {
                before_copy.erase(it2);
            }
        }
        for (const auto& s : before_copy) {
            diff.instruction_changes.push_back({id, IrDiff::InstructionChange::Removed, s, ""});
        }

        // Instructions added: in after but not before
        auto after_copy = after_set;
        for (const auto& s : before_set) {
            auto it2 = after_copy.find(s);
            if (it2 != after_copy.end()) {
                after_copy.erase(it2);
            }
        }
        for (const auto& s : after_copy) {
            diff.instruction_changes.push_back({id, IrDiff::InstructionChange::Added, "", s});
        }
    }

    return diff;
}

std::string format_diff(const IrDiff& diff, const char* stage_name,
                         std::size_t stage_ordinal,
                         const std::string& func_name,
                         std::uint64_t func_addr) {
    if (!diff.has_changes()) return "";

    std::ostringstream ss;
    ss << "=== " << stage_name;
    if (stage_ordinal > 0) ss << " (#" << stage_ordinal << ")";
    if (func_addr != 0) ss << std::format(" @0x{:x}", func_addr);
    if (!func_name.empty()) ss << " (" << func_name << ")";
    ss << " ===\n";

    // Block-level changes
    for (auto id : diff.blocks_removed) {
        ss << "Block bb_" << id << " REMOVED\n";
    }
    for (auto id : diff.blocks_added) {
        ss << "Block bb_" << id << " ADDED\n";
    }

    // Group instruction changes by block
    std::unordered_map<std::size_t, std::vector<const IrDiff::InstructionChange*>> by_block;
    for (const auto& change : diff.instruction_changes) {
        by_block[change.block_id].push_back(&change);
    }

    for (auto& [block_id, changes] : by_block) {
        ss << "bb_" << block_id << ":\n";
        for (auto* change : changes) {
            switch (change->kind) {
                case IrDiff::InstructionChange::Removed:
                    ss << "  - " << change->old_text << "\n";
                    break;
                case IrDiff::InstructionChange::Added:
                    ss << "  + " << change->new_text << "\n";
                    break;
                case IrDiff::InstructionChange::Modified:
                    ss << "  ~ " << change->old_text << " -> " << change->new_text << "\n";
                    break;
            }
        }
    }

    return ss.str();
}

} // namespace aletheia::debug
