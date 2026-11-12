#pragma once

#include "lob/engine/order_pool.hpp"  // kNil
#include "lob/types.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob::engine {

/// OrderId -> pool index, open addressing with linear probing.
///
/// - **Index-only slots (M6).** A slot holds just the 4-byte pool index; the
///   key is read back from the order itself through a `key_of(index)`
///   callable supplied by the caller. Every resting order already stores its
///   id, so the table needn't store it again. Slots are 4 B instead of 16 B
///   (16 per cache line, 4x less memory for the same capacity). A hit costs
///   nothing extra because the caller touches the order next anyway, and at
///   low occupancy a miss usually lands on an empty slot without touching
///   any order.
/// - Capacity is a power of two >= 2x the maximum number of live entries, so
///   the load factor never exceeds 0.5 and probe sequences stay short.
/// - Order ids are arbitrary client-supplied uint64s (0 and UINT64_MAX are
///   valid); an empty slot is marked by kNil, never by a reserved key.
/// - Deletion uses backward-shift instead of tombstones: no slow decay of probe
///   lengths under heavy add/cancel churn, and no periodic rehash.
/// - Keys are placed by Fibonacci hashing: multiply by 2^64/phi and keep the
///   top log2(capacity) bits (one multiply on the critical path).
///
/// All memory is allocated in the constructor; no operation touches the heap.
class IdMap {
public:
    explicit IdMap(std::size_t max_entries)
        : mask_(capacity_for(max_entries) - 1),
          shift_(64U - static_cast<unsigned>(std::countr_zero(mask_ + 1))),
          slots_(mask_ + 1, kNil) {}

    /// Result of probing for an id: the slot that holds it, or (if absent) the
    /// empty slot where it would be inserted.
    struct Probe {
        std::size_t slot;
        bool found;
    };

    /// `key_of(index)` returns the id (as uint64) of the entry with that value.
    template <class KeyOf>
    [[nodiscard]] Probe probe(OrderId id, const KeyOf& key_of) const noexcept {
        const std::uint64_t key = id.value();
        for (std::size_t i = home(key);; i = next(i)) {
            const std::uint32_t value = slots_[i];
            if (value == kNil) {
                return {.slot = i, .found = false};
            }
            if (key_of(value) == key) {
                return {.slot = i, .found = true};
            }
        }
    }

    [[nodiscard]] std::uint32_t value_at(std::size_t slot) const noexcept { return slots_[slot]; }

    /// Pool index for `id`, or kNil if absent.
    template <class KeyOf>
    [[nodiscard]] std::uint32_t find(OrderId id, const KeyOf& key_of) const noexcept {
        const Probe p = probe(id, key_of);
        return p.found ? slots_[p.slot] : kNil;
    }

    /// Insert into the empty slot returned by an earlier probe, skipping a
    /// second probe. Preconditions: the map has not been modified since that
    /// probe (an erase can shift entries and invalidate the slot), and
    /// key_of(value) already returns the new id.
    void insert_at(std::size_t slot, std::uint32_t value) noexcept {
        assert(value != kNil && slots_[slot] == kNil);
        slots_[slot] = value;
        ++size_;
    }

    /// Precondition: `id` is absent, key_of(value) == id, and the map is below
    /// its sizing limit.
    template <class KeyOf>
    void insert(OrderId id, std::uint32_t value, const KeyOf& key_of) noexcept {
        const Probe p = probe(id, key_of);
        assert(!p.found);
        insert_at(p.slot, value);
    }

    /// Precondition: `id` is present.
    template <class KeyOf>
    void erase(OrderId id, const KeyOf& key_of) noexcept {
        const Probe p = probe(id, key_of);
        assert(p.found);
        erase_at(p.slot, key_of);
    }

    /// Erase the entry in `slot` (from a probe that found it).
    template <class KeyOf>
    void erase_at(std::size_t slot, const KeyOf& key_of) noexcept {
        assert(slots_[slot] != kNil);
        // Backward shift: walk the cluster after the hole and pull back any
        // entry whose probe path passes through the hole.
        std::size_t hole = slot;
        std::size_t j = hole;
        for (;;) {
            j = next(j);
            if (slots_[j] == kNil) {
                break;
            }
            const std::size_t k = home(key_of(slots_[j]));
            // The entry at j (home k) may move to `hole` iff `hole` lies in the
            // cyclic interval [k, j), i.e. on its probe path.
            const bool on_path = hole < j ? (k <= hole || k > j) : (k <= hole && k > j);
            if (on_path) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole] = kNil;
        --size_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    [[nodiscard]] static std::size_t capacity_for(std::size_t max_entries) {
        return std::bit_ceil(std::max<std::size_t>(16, max_entries * 2));
    }

    // Fibonacci hashing: the high bits of key * 2^64/phi. Multiplication by an
    // odd constant is a bijection mod 2^64, and the top bits depend on every
    // bit of the key. (M6: replaced the murmur3 finalizer, which costs two
    // multiplies and three shift-xors.)
    static constexpr std::uint64_t kFibonacci = 0x9e3779b97f4a7c15ULL;
    [[nodiscard]] std::size_t home(std::uint64_t key) const noexcept {
        return (key * kFibonacci) >> shift_;
    }
    [[nodiscard]] std::size_t next(std::size_t i) const noexcept { return (i + 1) & mask_; }

    std::size_t mask_;
    unsigned shift_;  // 64 - log2(capacity)
    std::vector<std::uint32_t> slots_;
    std::size_t size_{0};
};

}  // namespace lob::engine
