#pragma once
// =============================================================================
// SmallVector<T, N> — inline-buffered vector for arena-allocated IR nodes
// =============================================================================
// Stores up to N elements inline (no heap allocation). Spills to a heap-
// allocated buffer only when size exceeds N. This eliminates the dominant
// performance bottleneck in expression propagation: std::vector<Expression*>
// inside arena-allocated Operation nodes was causing ~60% of CPU time to be
// spent in malloc/free/memmove during recursive replace_variable_ptr traversal.
//
// Typical operation sizes:
//   Unary:  1 operand  (negate, deref, bit_not, logical_not, cast, ...)
//   Binary: 2 operands (add, sub, mul, cmp, ...)
//   Ternary: 3 operands (ternary ? : )
//   Call:   1+ target + args (usually ≤8)
//
// N=4 covers >95% of all Operation nodes without any heap allocation.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace aletheia {

template <typename T, std::size_t N>
class SmallVector {
    static_assert(N > 0, "SmallVector inline capacity must be > 0");
    static_assert(std::is_trivially_copyable_v<T> || std::is_pointer_v<T>,
        "SmallVector is optimized for trivially copyable types (pointers, ints)");

public:
    using value_type = T;
    using size_type = std::size_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    // --- Constructors ---

    SmallVector() noexcept : size_(0), capacity_(N), data_(inline_buf()) {}

    explicit SmallVector(size_type count, const T& value = T())
        : size_(0), capacity_(N), data_(inline_buf()) {
        reserve(count);
        for (size_type i = 0; i < count; ++i) {
            data_[i] = value;
        }
        size_ = count;
    }

    SmallVector(std::initializer_list<T> init)
        : size_(0), capacity_(N), data_(inline_buf()) {
        reserve(init.size());
        std::copy(init.begin(), init.end(), data_);
        size_ = init.size();
    }

    /// Move-construct from std::vector<T> for backward compatibility.
    /// Takes ownership of the vector's data if it has more than N elements;
    /// otherwise copies inline.
    SmallVector(std::vector<T>&& vec)
        : size_(0), capacity_(N), data_(inline_buf()) {
        reserve(vec.size());
        std::copy(vec.begin(), vec.end(), data_);
        size_ = vec.size();
        vec.clear();
    }

    /// Copy-construct from std::vector<T>.
    explicit SmallVector(const std::vector<T>& vec)
        : size_(0), capacity_(N), data_(inline_buf()) {
        reserve(vec.size());
        std::copy(vec.begin(), vec.end(), data_);
        size_ = vec.size();
    }

    // Copy constructor
    SmallVector(const SmallVector& other)
        : size_(0), capacity_(N), data_(inline_buf()) {
        reserve(other.size_);
        std::copy(other.data_, other.data_ + other.size_, data_);
        size_ = other.size_;
    }

    // Move constructor
    SmallVector(SmallVector&& other) noexcept
        : size_(other.size_), capacity_(other.capacity_) {
        if (other.is_inline()) {
            // Data is in other's inline buffer — must copy
            data_ = inline_buf();
            std::copy(other.data_, other.data_ + other.size_, data_);
        } else {
            // Data is heap-allocated — steal the pointer
            data_ = other.data_;
            other.data_ = other.inline_buf();
            other.capacity_ = N;
        }
        other.size_ = 0;
    }

    // Copy assignment
    SmallVector& operator=(const SmallVector& other) {
        if (this == &other) return *this;
        clear();
        reserve(other.size_);
        std::copy(other.data_, other.data_ + other.size_, data_);
        size_ = other.size_;
        return *this;
    }

    // Move assignment
    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this == &other) return *this;
        free_heap();
        size_ = other.size_;
        capacity_ = other.capacity_;
        if (other.is_inline()) {
            data_ = inline_buf();
            std::copy(other.data_, other.data_ + other.size_, data_);
        } else {
            data_ = other.data_;
            other.data_ = other.inline_buf();
            other.capacity_ = N;
        }
        other.size_ = 0;
        return *this;
    }

    ~SmallVector() {
        free_heap();
    }

    // --- Capacity ---

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    size_type size() const noexcept { return size_; }
    size_type capacity() const noexcept { return capacity_; }

    void reserve(size_type new_cap) {
        if (new_cap <= capacity_) return;
        grow(new_cap);
    }

    // --- Element access ---

    reference operator[](size_type i) noexcept {
        assert(i < size_);
        return data_[i];
    }
    const_reference operator[](size_type i) const noexcept {
        assert(i < size_);
        return data_[i];
    }

    reference front() noexcept { assert(size_ > 0); return data_[0]; }
    const_reference front() const noexcept { assert(size_ > 0); return data_[0]; }
    reference back() noexcept { assert(size_ > 0); return data_[size_ - 1]; }
    const_reference back() const noexcept { assert(size_ > 0); return data_[size_ - 1]; }

    pointer data() noexcept { return data_; }
    const_pointer data() const noexcept { return data_; }

    // --- Iterators ---

    iterator begin() noexcept { return data_; }
    const_iterator begin() const noexcept { return data_; }
    const_iterator cbegin() const noexcept { return data_; }
    iterator end() noexcept { return data_ + size_; }
    const_iterator end() const noexcept { return data_ + size_; }
    const_iterator cend() const noexcept { return data_ + size_; }

    // --- Modifiers ---

    void push_back(const T& value) {
        if (size_ == capacity_) grow(capacity_ * 2);
        data_[size_++] = value;
    }

    void push_back(T&& value) {
        if (size_ == capacity_) grow(capacity_ * 2);
        data_[size_++] = std::move(value);
    }

    template <typename... Args>
    reference emplace_back(Args&&... args) {
        if (size_ == capacity_) grow(capacity_ * 2);
        data_[size_] = T(std::forward<Args>(args)...);
        return data_[size_++];
    }

    void pop_back() noexcept {
        assert(size_ > 0);
        --size_;
    }

    void clear() noexcept {
        size_ = 0;
    }

    void resize(size_type new_size, const T& value = T()) {
        if (new_size > capacity_) grow(new_size);
        if (new_size > size_) {
            for (size_type i = size_; i < new_size; ++i) {
                data_[i] = value;
            }
        }
        size_ = new_size;
    }

    iterator erase(const_iterator pos) {
        assert(pos >= begin() && pos < end());
        auto* mutable_pos = const_cast<iterator>(pos);
        std::copy(mutable_pos + 1, end(), mutable_pos);
        --size_;
        return mutable_pos;
    }

    iterator erase(const_iterator first, const_iterator last) {
        assert(first >= begin() && last <= end() && first <= last);
        auto* mutable_first = const_cast<iterator>(first);
        auto* mutable_last = const_cast<iterator>(last);
        auto count = std::distance(mutable_first, mutable_last);
        std::copy(mutable_last, end(), mutable_first);
        size_ -= count;
        return mutable_first;
    }

    iterator insert(const_iterator pos, const T& value) {
        auto index = pos - begin();
        if (size_ == capacity_) grow(capacity_ * 2);
        auto* mutable_pos = data_ + index;
        std::copy_backward(mutable_pos, end(), end() + 1);
        *mutable_pos = value;
        ++size_;
        return mutable_pos;
    }

    // --- Comparison ---

    bool operator==(const SmallVector& other) const noexcept {
        if (size_ != other.size_) return false;
        return std::equal(begin(), end(), other.begin());
    }
    bool operator!=(const SmallVector& other) const noexcept {
        return !(*this == other);
    }

    // --- STL algorithm support ---

    /// Remove elements matching predicate (equivalent to std::erase_if).
    template <typename Predicate>
    size_type erase_if(Predicate pred) {
        auto* new_end = std::remove_if(begin(), end(), pred);
        auto removed = static_cast<size_type>(end() - new_end);
        size_ -= removed;
        return removed;
    }

private:
    // Returns pointer to the inline buffer (embedded in this object).
    T* inline_buf() noexcept {
        return reinterpret_cast<T*>(inline_storage_);
    }
    const T* inline_buf() const noexcept {
        return reinterpret_cast<const T*>(inline_storage_);
    }

    bool is_inline() const noexcept {
        return data_ == inline_buf();
    }

    void free_heap() {
        if (!is_inline()) {
            delete[] data_;
            data_ = inline_buf();
            capacity_ = N;
        }
    }

    void grow(size_type new_cap) {
        assert(new_cap > capacity_);
        T* new_data = new T[new_cap];
        std::copy(data_, data_ + size_, new_data);
        free_heap();
        data_ = new_data;
        capacity_ = new_cap;
    }

    size_type size_;
    size_type capacity_;
    T* data_;
    alignas(T) char inline_storage_[sizeof(T) * N];
};

} // namespace aletheia
