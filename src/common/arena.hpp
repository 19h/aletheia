#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>

namespace dewolf {

class DecompilerArena {
public:
    DecompilerArena(std::size_t block_size = 64 * 1024);
    ~DecompilerArena() = default;

    // Delete copy and move semantics
    DecompilerArena(const DecompilerArena&) = delete;
    DecompilerArena& operator=(const DecompilerArena&) = delete;
    DecompilerArena(DecompilerArena&&) = delete;
    DecompilerArena& operator=(DecompilerArena&&) = delete;

    void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* ptr = allocate(sizeof(T), alignof(T));
        return new (ptr) T(std::forward<Args>(args)...);
    }

    void reset();

private:
    struct Block {
        std::vector<uint8_t> data;
        std::size_t used = 0;

        explicit Block(std::size_t size) : data(size) {}
    };

    std::size_t block_size_;
    std::vector<std::unique_ptr<Block>> blocks_;
    std::size_t current_block_ = 0;

    void add_block(std::size_t min_size);
};

} // namespace dewolf
