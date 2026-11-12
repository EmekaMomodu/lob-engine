#pragma once

// Design B: one flat array of price levels, indexed by tick offset within the
// configured price band.
//
//   levels_[i] is the level at price min_price + i (either side).
//
// Locating a level is an index computation: no tree, no hashing, no pointer
// chasing, and levels near the mid sit next to each other in memory. One array
// serves both sides because the book is never crossed between commands: a
// tick can hold a bid level or an ask level, never both.
//
// The cost moves elsewhere: when the best level empties, the next one has to
// be *found*. Two strategies, chosen at compile time, so they can be compared:
//   - BestSearch::Bitmap : per-side two-level occupancy bitmap (O(1)-ish).
//   - BestSearch::Linear : walk the Level array tick by tick (O(gap)).
//
// The price band must be bounded (kMaxBandTicks); real venues enforce price
// bands / collars, and recentring the window is listed as future work.

#include "lob/engine/level.hpp"
#include "lob/engine/occupancy_bitmap.hpp"
#include "lob/engine/order_pool.hpp"
#include "lob/types.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace lob::engine {

enum class BestSearch : std::uint8_t { Bitmap, Linear };

/// Widest band design B accepts: 4M ticks = 128 MB of levels.
inline constexpr std::size_t kMaxBandTicks = std::size_t{1} << 22U;

template <BestSearch Search>
class ArrayLevels {
public:
    explicit ArrayLevels(const BookConfig& config)
        : min_price_(config.min_price),
          levels_(band_ticks(config)),
          bits_{OccupancyBitmap(bitmap_slots()), OccupancyBitmap(bitmap_slots())} {
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            levels_[i].price = Price{min_price_.value() + static_cast<std::int64_t>(i)};
        }
    }

    [[nodiscard]] LevelRef best(Side side) const noexcept { return best_[index(side)]; }

    [[nodiscard]] Level& at(LevelRef ref) noexcept { return levels_[ref]; }
    [[nodiscard]] const Level& at(LevelRef ref) const noexcept { return levels_[ref]; }

    /// Precondition: price is inside the band (the engine validates this).
    [[nodiscard]] LevelRef get_or_insert(Side side, Price price) noexcept {
        assert(price >= min_price_
               && price.value() - min_price_.value() < static_cast<std::int64_t>(levels_.size()));
        const auto ref = static_cast<LevelRef>(price.value() - min_price_.value());
        Level& level = levels_[ref];
        if (level.count == 0) {
            assert(level.head == kNil && level.tail == kNil && level.total == 0);
            level.side = side;
            occupy(side, ref);
        }
        assert(level.side == side);  // the book is never crossed
        return ref;
    }

    void erase(LevelRef ref) noexcept {
        const Level& level = levels_[ref];
        assert(level.count == 0);
        const Side side = level.side;
        if constexpr (Search == BestSearch::Bitmap) {
            bits_[index(side)].clear(ref);
        }
        --occupied_[index(side)];
        if (ref == best_[index(side)]) {
            best_[index(side)] =
                side == Side::Buy ? lower_occupied(side, ref) : higher_occupied(side, ref);
        }
    }

    /// Visit levels on `side` best-first: f(LevelRef, const Level&).
    template <class F>
    void for_each(Side side, const F& visit) const {
        for (LevelRef ref = best_[index(side)]; ref != kNoLevel;
             ref = side == Side::Buy ? lower_occupied(side, ref) : higher_occupied(side, ref)) {
            visit(ref, levels_[ref]);
        }
    }

private:
    [[nodiscard]] static constexpr std::size_t index(Side side) noexcept {
        return static_cast<std::size_t>(side);
    }

    [[nodiscard]] static std::size_t band_ticks(const BookConfig& config) {
        if (config.min_price > config.max_price) {
            throw std::invalid_argument("BookConfig: min_price > max_price");
        }
        // Unsigned subtraction is exact here because max >= min.
        const std::uint64_t width = static_cast<std::uint64_t>(config.max_price.value())
                                    - static_cast<std::uint64_t>(config.min_price.value());
        if (width >= kMaxBandTicks) {
            throw std::invalid_argument("BookConfig: price band too wide for ArrayLevels");
        }
        return static_cast<std::size_t>(width) + 1;
    }

    [[nodiscard]] std::size_t bitmap_slots() const noexcept {
        return Search == BestSearch::Bitmap ? levels_.size() : 0;
    }

    void occupy(Side side, LevelRef ref) noexcept {
        if constexpr (Search == BestSearch::Bitmap) {
            bits_[index(side)].set(ref);
        }
        ++occupied_[index(side)];
        LevelRef& best = best_[index(side)];
        if (best == kNoLevel || (side == Side::Buy ? ref > best : ref < best)) {
            best = ref;
        }
    }

    /// Nearest occupied level on `side` strictly below `from`.
    [[nodiscard]] LevelRef lower_occupied(Side side, LevelRef from) const noexcept {
        if (from == 0 || occupied_[index(side)] == 0) {
            return kNoLevel;
        }
        if constexpr (Search == BestSearch::Bitmap) {
            const std::size_t found = bits_[index(side)].find_prev(from - 1);
            return found == OccupancyBitmap::kNpos ? kNoLevel : static_cast<LevelRef>(found);
        } else {
            for (LevelRef i = from; i-- > 0;) {
                if (levels_[i].count != 0 && levels_[i].side == side) {
                    return i;
                }
            }
            return kNoLevel;
        }
    }

    /// Nearest occupied level on `side` strictly above `from`.
    [[nodiscard]] LevelRef higher_occupied(Side side, LevelRef from) const noexcept {
        if (occupied_[index(side)] == 0) {
            return kNoLevel;
        }
        if constexpr (Search == BestSearch::Bitmap) {
            const std::size_t found = bits_[index(side)].find_next(std::size_t{from} + 1);
            return found == OccupancyBitmap::kNpos ? kNoLevel : static_cast<LevelRef>(found);
        } else {
            for (std::size_t i = std::size_t{from} + 1; i < levels_.size(); ++i) {
                if (levels_[i].count != 0 && levels_[i].side == side) {
                    return static_cast<LevelRef>(i);
                }
            }
            return kNoLevel;
        }
    }

    Price min_price_;
    std::vector<Level> levels_;
    std::array<OccupancyBitmap, 2> bits_;  // [Buy, Sell]; empty for BestSearch::Linear
    std::array<LevelRef, 2> best_{kNoLevel, kNoLevel};
    std::array<std::size_t, 2> occupied_{0, 0};  // number of non-empty levels per side
};

static_assert(PriceLevels<ArrayLevels<BestSearch::Bitmap>>);
static_assert(PriceLevels<ArrayLevels<BestSearch::Linear>>);

}  // namespace lob::engine
