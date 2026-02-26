#pragma once
#include "arena.hpp"
#include <new>

namespace dewolf {

class ArenaAllocated {
public:
    static void* operator new(std::size_t size, void* ptr) noexcept {
        return ptr;
    }
    
    // Disallow normal allocation
    static void* operator new(std::size_t size) = delete;
    static void* operator new[](std::size_t size) = delete;

    static void operator delete(void* ptr) noexcept {
        // No-op for arena allocations
    }
    static void operator delete(void* ptr, void* place) noexcept {
        // No-op for arena allocations
    }
};

} // namespace dewolf
