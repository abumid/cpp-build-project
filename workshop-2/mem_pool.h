#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std;

#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
#define MEMPOOL_DELETED(msg) delete (msg)
#else
#define MEMPOOL_DELETED(msg) delete
#endif

template <typename T>
class MemPool final {
public:
    explicit MemPool(size_t num_elems)
        : storage_(num_elems), in_use_(num_elems, 0), free_stack_(num_elems), free_count_(num_elems) {
        assert(num_elems > 0 && "pool must have at least one slot");
        assert(num_elems <= numeric_limits<Index>::max() && "pool too large for 32-bit indices");
        for (size_t i = 0; i < num_elems; ++i) {
            free_stack_[i] = static_cast<Index>(num_elems - 1 - i);
        }
    }

    ~MemPool() {
        if constexpr (!is_trivially_destructible_v<T>) {
            for (size_t i = 0; i < storage_.size(); ++i) {
                if (in_use_[i]) destroy_at(slotPtr(i));
            }
        }
    }

    template <typename... Args>
    [[nodiscard]] T *allocate(Args &&...args) noexcept {
        if (free_count_ == 0) [[unlikely]] {
            return nullptr;
        }
        const Index idx = free_stack_[--free_count_];
        assert(!in_use_[idx] && "free stack handed out a slot that is in use");
        in_use_[idx] = 1;
        return construct_at(slotPtr(idx), static_cast<Args &&>(args)...);
    }

    void deallocate(const T *elem) noexcept {
        if (elem == nullptr) return;
        const size_t idx = indexOf(elem);
        assert(idx < storage_.size() && "pointer does not belong to this pool");
        assert(in_use_[idx] && "double free (slot is already free)");
        destroy_at(const_cast<T *>(elem));
        in_use_[idx] = 0;
        free_stack_[free_count_++] = static_cast<Index>(idx);
    }

    [[nodiscard]] bool owns(const T *elem) const noexcept {
        const auto *p = reinterpret_cast<const byte *>(elem);
        const auto *begin = reinterpret_cast<const byte *>(storage_.data());
        const auto *end = begin + storage_.size() * sizeof(Slot);
        return p >= begin && p < end && (p - begin) % sizeof(Slot) == 0;
    }

    [[nodiscard]] size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] size_t available() const noexcept { return free_count_; }
    [[nodiscard]] size_t size() const noexcept { return capacity() - free_count_; }

    MemPool() = MEMPOOL_DELETED("a MemPool needs a fixed capacity");
    MemPool(const MemPool &) = MEMPOOL_DELETED("a MemPool owns its backing storage and free list");
    MemPool(MemPool &&) = MEMPOOL_DELETED("a MemPool owns its backing storage and free list");
    MemPool &operator=(const MemPool &) = MEMPOOL_DELETED("a MemPool owns its backing storage and free list");
    MemPool &operator=(MemPool &&) = MEMPOOL_DELETED("a MemPool owns its backing storage and free list");

private:
    using Index = uint32_t;

    struct alignas(T) Slot {
        byte bytes[sizeof(T)];
    };
    static_assert(sizeof(Slot) == sizeof(T));

    T *slotPtr(size_t idx) noexcept { return reinterpret_cast<T *>(storage_[idx].bytes); }

    size_t indexOf(const T *elem) const noexcept {
        const auto *p = reinterpret_cast<const byte *>(elem);
        const auto *begin = reinterpret_cast<const byte *>(storage_.data());
        return static_cast<size_t>(p - begin) / sizeof(Slot);
    }

    vector<Slot> storage_;
    vector<uint8_t> in_use_;
    vector<Index> free_stack_;
    size_t free_count_;
};

#undef MEMPOOL_DELETED
