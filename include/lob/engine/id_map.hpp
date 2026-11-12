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
/// - Capacity is a power of two >= 2x the maximum number of live entries, so
///   the load factor never exceeds 0.5 and probe sequences stay short.
/// - Order ids are arbitrary client-supplied uint64s (0 and UINT64_MAX are
///   valid), so emptiness is marked in the *value* (kNil), not the key.
/// - Deletion uses backward-shift instead of tombstones: no slow decay of probe
///   lengths under a heavy add/cancel churn, and no periodic rehash.
/// - Keys go through the murmur3 64-bit finalizer, so sequential ids (the
///   common case) spread evenly instead of clustering.
///
/// All memory is allocated in the constructor; no operation touches the heap.
class IdMap {
public:
    explicit IdMap(std::size_t max_entries)
        : mask_(capacity_for(max_entries) - 1), slots_(mask_ + 1) {}

    /// Pool index for `id`, or kNil if absent.
    [[nodiscard]] std::uint32_t find(OrderId id) const noexcept {
        const std::uint64_t key = id.value();
        for (std::size_t i = home(key);; i = next(i)) {
            const Slot& slot = slots_[i];
            if (slot.value == kNil) {
                return kNil;
            }
            if (slot.key == key) {
                return slot.value;
            }
        }
    }

    /// Precondition: `id` is absent and the map is below its sizing limit.
    void insert(OrderId id, std::uint32_t value) noexcept {
        assert(value != kNil);
        const std::uint64_t key = id.value();
        std::size_t i = home(key);
        while (slots_[i].value != kNil) {
            assert(slots_[i].key != key);
            i = next(i);
        }
        slots_[i] = Slot{.key = key, .value = value};
        ++size_;
    }

    /// Precondition: `id` is present.
    void erase(OrderId id) noexcept {
        const std::uint64_t key = id.value();
        std::size_t hole = home(key);
        while (slots_[hole].key != key || slots_[hole].value == kNil) {
            assert(slots_[hole].value != kNil);  // key must be present
            hole = next(hole);
        }

        // Backward shift: walk the cluster after the hole and pull back any
        // entry whose probe path passes through the hole.
        std::size_t j = hole;
        for (;;) {
            j = next(j);
            if (slots_[j].value == kNil) {
                break;
            }
            const std::size_t k = home(slots_[j].key);
            // The entry at j (home k) may move to `hole` iff `hole` lies in the
            // cyclic interval [k, j), i.e. on its probe path.
            const bool on_path = hole < j ? (k <= hole || k > j) : (k <= hole && k > j);
            if (on_path) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].value = kNil;
        --size_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        std::uint64_t key{};
        std::uint32_t value{kNil};
    };
    static_assert(sizeof(Slot) == 16);

    [[nodiscard]] static std::size_t capacity_for(std::size_t max_entries) {
        return std::bit_ceil(std::max<std::size_t>(16, max_entries * 2));
    }

    // murmur3 fmix64: a cheap bijective mixer with full avalanche.
    [[nodiscard]] static constexpr std::uint64_t mix(std::uint64_t x) noexcept {
        x ^= x >> 33U;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33U;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33U;
        return x;
    }

    [[nodiscard]] std::size_t home(std::uint64_t key) const noexcept { return mix(key) & mask_; }
    [[nodiscard]] std::size_t next(std::size_t i) const noexcept { return (i + 1) & mask_; }

    std::size_t mask_;
    std::vector<Slot> slots_;
    std::size_t size_{0};
};

}  // namespace lob::engine
