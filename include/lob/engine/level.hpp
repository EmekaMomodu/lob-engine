#pragma once

#include "lob/engine/order_pool.hpp"  // kNil
#include "lob/types.hpp"

#include <concepts>
#include <cstdint>

namespace lob::engine {

/// Handle to a price level, owned by a PriceLevels implementation.
using LevelRef = std::uint32_t;
inline constexpr LevelRef kNoLevel = kNil;

/// One price level: the head/tail of an intrusive FIFO list of orders plus
/// aggregates. 32 bytes, 32-byte aligned.
///
///   offset  size  field
///        0     8  price
///        8     8  total   sum of leaves (64-bit: many orders x 1e9 overflows 32)
///       16     4  head    first order in time priority (kNil if empty)
///       20     4  tail    last order (kNil if empty)
///       24     4  count   number of orders
///       28     1  side
///       29     3  (padding)
struct alignas(32) Level {
    Price price;
    std::uint64_t total{};
    std::uint32_t head{kNil};
    std::uint32_t tail{kNil};
    std::uint32_t count{};
    Side side{};
};
static_assert(sizeof(Level) == 32);

/// What the matching engine needs from a price-level container. This is the
/// one thing that differs between design A (tree of levels) and design B
/// (flat array indexed by tick); the matching logic is shared.
template <class L>
concept PriceLevels =
    std::constructible_from<L, const BookConfig&>
    && requires(L& levels, const L& clevels, Side side, Price price, LevelRef ref) {
           /// Level at `price` on `side`, created empty if absent.
           { levels.get_or_insert(side, price) } -> std::same_as<LevelRef>;
           { levels.at(ref) } -> std::same_as<Level&>;
           { clevels.at(ref) } -> std::same_as<const Level&>;
           /// Remove a level. Precondition: it is empty.
           levels.erase(ref);
           /// Best level on `side`, or kNoLevel if that side is empty.
           { clevels.best(side) } -> std::same_as<LevelRef>;
       };

}  // namespace lob::engine
