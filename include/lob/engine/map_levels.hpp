#pragma once

// Design A: price levels in a balanced tree (std::map), one per side.
//
// The "obvious production" baseline: O(log L) to create or remove a level,
// O(1) to read the best level (std::map caches its leftmost node). Two things
// keep it on the no-allocation hot path:
//   - map nodes come from a pre-sized BlockArena via ArenaAllocator, and
//   - Level structs live in a separate contiguous pool; the map stores only a
//     32-bit handle, so orders can point at their level in O(1) (cancel never
//     searches the tree unless the level becomes empty).

#include "lob/engine/block_arena.hpp"
#include "lob/engine/level.hpp"
#include "lob/engine/order_pool.hpp"
#include "lob/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace lob::engine {

class MapLevels {
public:
    explicit MapLevels(const BookConfig& config)
        : arena_(kNodeBlockSize, kNodeBlockAlign, config.max_orders),
          bids_(std::greater<>{}, NodeAllocator{arena_}),
          asks_(std::less<>{}, NodeAllocator{arena_}),
          levels_(config.max_orders) {
        // A level only exists while at least one order rests on it, so the
        // number of levels never exceeds max_orders (and neither does the
        // number of map nodes drawn from the arena).
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            levels_[i].head = i + 1 < levels_.size() ? static_cast<std::uint32_t>(i + 1) : kNil;
        }
        free_head_ = levels_.empty() ? kNil : 0;
    }

    // The maps' allocators point at arena_, so the object must stay put.
    MapLevels(const MapLevels&) = delete;
    MapLevels& operator=(const MapLevels&) = delete;
    MapLevels(MapLevels&&) = delete;
    MapLevels& operator=(MapLevels&&) = delete;
    ~MapLevels() = default;

    [[nodiscard]] LevelRef best(Side side) const noexcept {
        if (side == Side::Buy) {
            return bids_.empty() ? kNoLevel : bids_.begin()->second;
        }
        return asks_.empty() ? kNoLevel : asks_.begin()->second;
    }

    [[nodiscard]] Level& at(LevelRef ref) noexcept { return levels_[ref]; }
    [[nodiscard]] const Level& at(LevelRef ref) const noexcept { return levels_[ref]; }

    [[nodiscard]] LevelRef get_or_insert(Side side, Price price) {
        return side == Side::Buy ? get_or_insert_in(bids_, side, price)
                                 : get_or_insert_in(asks_, side, price);
    }

    void erase(LevelRef ref) noexcept {
        Level& level = levels_[ref];
        assert(level.count == 0);
        if (level.side == Side::Buy) {
            bids_.erase(level.price);
        } else {
            asks_.erase(level.price);
        }
        level.head = free_head_;  // free-list link
        free_head_ = ref;
    }

    /// Visit levels on `side` best-first: f(LevelRef, const Level&).
    template <class F>
    void for_each(Side side, const F& visit) const {
        if (side == Side::Buy) {
            for (const auto& [price, ref] : bids_) {
                visit(ref, levels_[ref]);
            }
        } else {
            for (const auto& [price, ref] : asks_) {
                visit(ref, levels_[ref]);
            }
        }
    }

private:
    // Big enough for a red-black tree node holding pair<const Price, LevelRef>
    // in both libstdc++ and libc++ (48 bytes); ArenaAllocator throws if not.
    static constexpr std::size_t kNodeBlockSize = 64;
    static constexpr std::size_t kNodeBlockAlign = 16;

    using NodeAllocator = ArenaAllocator<std::pair<const Price, LevelRef>>;
    using BidMap = std::map<Price, LevelRef, std::greater<>, NodeAllocator>;  // begin() = best
    using AskMap = std::map<Price, LevelRef, std::less<>, NodeAllocator>;     // begin() = best

    template <class Map>
    LevelRef get_or_insert_in(Map& map, Side side, Price price) {
        const auto [it, inserted] = map.try_emplace(price, kNoLevel);
        if (inserted) {
            const LevelRef ref = free_head_;
            assert(ref != kNil);
            free_head_ = levels_[ref].head;
            levels_[ref] = Level{
                .price = price, .total = 0, .head = kNil, .tail = kNil, .count = 0, .side = side};
            it->second = ref;
        }
        return it->second;
    }

    BlockArena arena_;  // must outlive (be declared before) the maps
    BidMap bids_;
    AskMap asks_;
    std::vector<Level> levels_;
    LevelRef free_head_{kNil};
};

static_assert(PriceLevels<MapLevels>);

}  // namespace lob::engine
