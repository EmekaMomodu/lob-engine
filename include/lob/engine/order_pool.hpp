#pragma once

#include "lob/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lob::engine {

/// "No index" sentinel for 32-bit pool / level handles.
inline constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

/// A resting order. 32 bytes, 32-byte aligned: two per 64 B cache line (four
/// per 128 B line on Apple Silicon), and never split across a line.
///
/// Links are 32-bit pool indices rather than pointers: half the size, and the
/// whole pool is one contiguous allocation. The order's price and side are
/// reachable through its level, but `side` lives here too because it fits in
/// what would otherwise be padding.
///
///   offset  size  field
///        0     8  id
///        8     4  leaves   open quantity
///       12     4  total    total order quantity incl. filled (modify semantics)
///       16     4  prev     previous order at the same level (kNil = head)
///       20     4  next     next order at the same level (kNil = tail); free-list link
///       24     4  level    handle of the level this order rests at
///       28     1  side
///       29     3  (padding)
struct alignas(32) Order {
    OrderId id;
    std::uint32_t leaves{};
    std::uint32_t total{};
    std::uint32_t prev{kNil};
    std::uint32_t next{kNil};
    std::uint32_t level{kNil};
    Side side{};
};
static_assert(sizeof(Order) == 32);
static_assert(alignof(Order) == 32);

/// Fixed-capacity slab of orders with an intrusive LIFO free list.
///
/// All memory is allocated (and touched) in the constructor; allocate() and
/// release() are O(1) and never call the heap. LIFO reuse hands back the most
/// recently freed slot, which is the one most likely to still be in cache.
class OrderPool {
public:
    explicit OrderPool(std::size_t capacity) : orders_(capacity) {
        assert(capacity < kNil);
        for (std::size_t i = 0; i < capacity; ++i) {
            orders_[i].next = i + 1 < capacity ? static_cast<std::uint32_t>(i + 1) : kNil;
        }
        free_head_ = capacity > 0 ? 0 : kNil;
    }

    /// Precondition: live() < capacity().
    [[nodiscard]] std::uint32_t allocate() noexcept {
        const std::uint32_t index = free_head_;
        assert(index != kNil);
        free_head_ = orders_[index].next;
        ++live_;
        return index;
    }

    void release(std::uint32_t index) noexcept {
        assert(index < orders_.size());
        orders_[index].next = free_head_;
        free_head_ = index;
        --live_;
    }

    [[nodiscard]] Order& operator[](std::uint32_t index) noexcept { return orders_[index]; }
    [[nodiscard]] const Order& operator[](std::uint32_t index) const noexcept {
        return orders_[index];
    }

    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return orders_.size(); }

private:
    std::vector<Order> orders_;
    std::uint32_t free_head_{kNil};
    std::size_t live_{0};
};

}  // namespace lob::engine
