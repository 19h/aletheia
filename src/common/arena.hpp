#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <type_traits>

namespace aletheia {

class DecompilerArena {
public:
    DecompilerArena(std::size_t block_size = 1024 * 1024);
    ~DecompilerArena() { reset(); }

    // Delete copy and move semantics
    DecompilerArena(const DecompilerArena&) = delete;
    DecompilerArena& operator=(const DecompilerArena&) = delete;
    DecompilerArena(DecompilerArena&&) = delete;
    DecompilerArena& operator=(DecompilerArena&&) = delete;

    void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* ptr = allocate(sizeof(T), alignof(T));
        T* obj = new (ptr) T(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            destructors_.push_back({[](void* p) { static_cast<T*>(p)->~T(); }, obj});
        }
        return obj;
    }

    void reset();

private:
    struct DtorNode {
        void (*dtor)(void*);
        void* obj;
    };
    std::vector<DtorNode> destructors_;

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

} // namespace aletheia
