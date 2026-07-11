#include "arena.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace aletheia {

DecompilerArena::DecompilerArena(std::size_t block_size)
    : block_size_(std::max<std::size_t>(block_size, 1)) {
    add_block(block_size_);
}

void* DecompilerArena::allocate(std::size_t size, std::size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        throw std::invalid_argument("DecompilerArena alignment must be a non-zero power of two");
    }
    if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        throw std::bad_alloc();
    }

    if (blocks_.empty()) {
        add_block(std::max(size + alignment - 1, block_size_));
    }

    Block* block = nullptr;
    std::size_t offset = 0;
    for (;;) {
        block = blocks_[current_block_].get();
        const std::size_t current_addr =
            reinterpret_cast<std::size_t>(block->data.data() + block->used);
        offset = (alignment - (current_addr % alignment)) % alignment;
        const std::size_t available = block->data.size() - block->used;
        if (offset <= available && size <= available - offset) {
            break;
        }

        // A reset arena can contain several previously allocated blocks of
        // different sizes. Scan all reusable blocks and re-check capacity at
        // each one before growing the arena.
        if (current_block_ + 1 < blocks_.size()) {
            current_block_++;
        } else {
            add_block(std::max(size + alignment - 1, block_size_));
        }
    }

    block->used += offset;
    void* result = block->data.data() + block->used;
    block->used += size;

    return result;
}

void DecompilerArena::reset() {
    // Call registered destructors in reverse order (LIFO) to ensure proper
    // teardown of objects that reference other arena-allocated objects.
    // This is critical: arena-allocated types with non-trivial members
    // (std::vector, std::string, std::unordered_map, z3::expr, etc.) will
    // leak their heap-allocated internal buffers if destructors are not called.
    for (auto it = destructors_.rbegin(); it != destructors_.rend(); ++it) {
        it->dtor(it->obj);
    }
    destructors_.clear();

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
