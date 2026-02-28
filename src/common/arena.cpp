#include "arena.hpp"
#include <algorithm>

namespace aletheia {

DecompilerArena::DecompilerArena(std::size_t block_size)
    : block_size_(block_size) {
    add_block(block_size_);
}

void* DecompilerArena::allocate(std::size_t size, std::size_t alignment) {
    if (blocks_.empty()) {
        add_block(std::max(size, block_size_));
    }

    Block* block = blocks_[current_block_].get();
    
    // Calculate alignment adjustment
    std::size_t current_addr = reinterpret_cast<std::size_t>(block->data.data() + block->used);
    std::size_t offset = (alignment - (current_addr % alignment)) % alignment;

    if (block->used + offset + size > block->data.size()) {
        // Need a new block
        if (current_block_ + 1 < blocks_.size()) {
            current_block_++;
            block = blocks_[current_block_].get();
        } else {
            add_block(std::max(size + alignment, block_size_));
            block = blocks_[current_block_].get();
        }
        
        current_addr = reinterpret_cast<std::size_t>(block->data.data() + block->used);
        offset = (alignment - (current_addr % alignment)) % alignment;
    }

    block->used += offset;
    void* result = block->data.data() + block->used;
    block->used += size;

    return result;
}

void DecompilerArena::reset() {
    for (auto& block : blocks_) {
        block->used = 0;
    }
    current_block_ = 0;
}

void DecompilerArena::add_block(std::size_t min_size) {
    blocks_.push_back(std::make_unique<Block>(min_size));
    current_block_ = blocks_.size() - 1;
}

} // namespace aletheia
