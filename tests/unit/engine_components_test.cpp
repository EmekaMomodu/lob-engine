// Unit tests for the engine's building blocks: IdMap, OrderPool, BlockArena,
// OccupancyBitmap, ArrayLevels.

#include "lob/engine/array_levels.hpp"
#include "lob/engine/block_arena.hpp"
#include "lob/engine/id_map.hpp"
#include "lob/engine/occupancy_bitmap.hpp"
#include "lob/engine/order_pool.hpp"
#include "lob/workload/rng.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace lob::engine {
namespace {

// --- IdMap ------------------------------------------------------------------

TEST(IdMap, InsertFindErase) {
    IdMap map(8);
    EXPECT_EQ(map.find(OrderId{1}), kNil);
    map.insert(OrderId{1}, 10);
    map.insert(OrderId{0}, 20);           // 0 is a valid id
    map.insert(OrderId{UINT64_MAX}, 30);  // so is the max value
    EXPECT_EQ(map.find(OrderId{1}), 10U);
    EXPECT_EQ(map.find(OrderId{0}), 20U);
    EXPECT_EQ(map.find(OrderId{UINT64_MAX}), 30U);
    EXPECT_EQ(map.size(), 3U);
    map.erase(OrderId{0});
    EXPECT_EQ(map.find(OrderId{0}), kNil);
    EXPECT_EQ(map.find(OrderId{1}), 10U);
    EXPECT_EQ(map.size(), 2U);
}

TEST(IdMap, CapacityIsPowerOfTwoAtLeastTwiceMaxEntries) {
    EXPECT_EQ(IdMap(1).capacity(), 16U);
    EXPECT_EQ(IdMap(100).capacity(), 256U);
    EXPECT_EQ(IdMap(128).capacity(), 256U);
    EXPECT_EQ(IdMap(129).capacity(), 512U);
}

// Randomised against std::unordered_map at full load. A small map makes long
// clusters and wrap-around common, which is where backward-shift deletion
// can go wrong.
void fuzz_id_map(std::size_t max_entries, std::uint64_t key_space, std::uint64_t seed) {
    IdMap map(max_entries);
    std::unordered_map<std::uint64_t, std::uint32_t> model;
    std::vector<std::uint64_t> keys;
    workload::Rng rng(seed);
    for (int step = 0; step < 200'000; ++step) {
        const bool can_insert = model.size() < max_entries;
        if (can_insert && (model.empty() || rng.coin())) {
            const std::uint64_t key = rng.below(key_space);
            if (model.contains(key)) {
                continue;
            }
            const auto value = static_cast<std::uint32_t>(rng.below(kNil));
            map.insert(OrderId{key}, value);
            model.emplace(key, value);
            keys.push_back(key);
        } else {
            const std::size_t i = rng.below(keys.size());
            const std::uint64_t key = keys[i];
            keys[i] = keys.back();
            keys.pop_back();
            map.erase(OrderId{key});
            model.erase(key);
        }
        // Spot-check a present key and an absent-or-present random key.
        if (!keys.empty()) {
            const std::uint64_t key = keys[rng.below(keys.size())];
            ASSERT_EQ(map.find(OrderId{key}), model.at(key)) << "step " << step;
        }
        const std::uint64_t probe = rng.below(key_space);
        const auto it = model.find(probe);
        ASSERT_EQ(map.find(OrderId{probe}), it == model.end() ? kNil : it->second)
            << "step " << step;
        ASSERT_EQ(map.size(), model.size());
    }
    for (const auto& [key, value] : model) {
        ASSERT_EQ(map.find(OrderId{key}), value);
    }
}

TEST(IdMap, MatchesModelSmallMapHeavyCollisions) {
    fuzz_id_map(8, 64, 1);
}
TEST(IdMap, MatchesModelMediumMap) {
    fuzz_id_map(500, 5'000, 2);
}
TEST(IdMap, MatchesModelSparseKeys) {
    fuzz_id_map(200, UINT64_MAX, 3);
}

// --- OrderPool ----------------------------------------------------------------

TEST(OrderPool, AllocatesEverySlotOnceThenReusesLifo) {
    OrderPool pool(4);
    std::vector<std::uint32_t> got;
    got.reserve(4);
    for (int i = 0; i < 4; ++i) {
        got.push_back(pool.allocate());
    }
    EXPECT_EQ(got, (std::vector<std::uint32_t>{0, 1, 2, 3}));
    EXPECT_EQ(pool.live(), 4U);
    pool.release(2);
    pool.release(0);
    EXPECT_EQ(pool.allocate(), 0U);  // most recently freed first
    EXPECT_EQ(pool.allocate(), 2U);
    EXPECT_EQ(pool.live(), 4U);
}

TEST(OrderPool, OrderLayout) {
    EXPECT_EQ(sizeof(Order), 32U);
    EXPECT_EQ(alignof(Order), 32U);
    OrderPool pool(3);
    const auto a = std::bit_cast<std::uintptr_t>(&pool[0]);
    EXPECT_EQ(a % 32, 0U) << "orders must not straddle cache lines";
    EXPECT_EQ(std::bit_cast<std::uintptr_t>(&pool[1]) - a, 32U);
}

// --- BlockArena / ArenaAllocator -----------------------------------------------

TEST(BlockArena, HandsOutDistinctAlignedBlocksAndRecyclesThem) {
    BlockArena arena(40, 16, 3);
    EXPECT_EQ(arena.block_size(), 48U);  // rounded up to alignment
    void* a = arena.allocate();
    void* b = arena.allocate();
    void* c = arena.allocate();
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    for (const void* p : {a, b, c}) {
        EXPECT_EQ(std::bit_cast<std::uintptr_t>(p) % 16, 0U);
    }
    EXPECT_THROW((void)arena.allocate(), std::bad_alloc);
    arena.deallocate(b);
    EXPECT_EQ(arena.allocate(), b);
    EXPECT_EQ(arena.in_use(), 3U);
}

TEST(ArenaAllocator, BacksStdMapUpToCapacity) {
    BlockArena arena(64, 16, 100);
    using Alloc = ArenaAllocator<std::pair<const int, int>>;
    std::map<int, int, std::less<>, Alloc> map{std::less<>{}, Alloc{arena}};
    for (int i = 0; i < 100; ++i) {
        map.emplace(i, i * i);
    }
    EXPECT_EQ(arena.in_use(), 100U);
    EXPECT_THROW(map.emplace(1000, 0), std::bad_alloc);  // arena exhausted
    map.erase(5);
    map.emplace(1000, 1);  // freed node is reused
    EXPECT_EQ(map.at(1000), 1);
    map.clear();
    EXPECT_EQ(arena.in_use(), 0U);
}

// --- OccupancyBitmap ----------------------------------------------------------

// Randomised against std::set at sizes around word (64) and summary (4096)
// boundaries, where off-by-one errors in the masks would show up.
void fuzz_bitmap(std::size_t slots, std::uint64_t seed) {
    OccupancyBitmap bits(slots);
    std::set<std::size_t> model;
    workload::Rng rng(seed);
    const auto expected_prev = [&](std::size_t i) -> std::size_t {
        const auto it = model.upper_bound(i);
        return it == model.begin() ? OccupancyBitmap::kNpos : *std::prev(it);
    };
    const auto expected_next = [&](std::size_t i) -> std::size_t {
        const auto it = model.lower_bound(i);
        return it == model.end() ? OccupancyBitmap::kNpos : *it;
    };
    for (int step = 0; step < 50'000; ++step) {
        const std::size_t i = rng.below(slots);
        // Bias towards sparse sets so long empty stretches are common.
        if (rng.below(100) < 40) {
            bits.set(i);
            model.insert(i);
        } else {
            bits.clear(i);
            model.erase(i);
        }
        const std::size_t q = rng.below(slots);
        ASSERT_EQ(bits.test(q), model.contains(q)) << "slots=" << slots << " step=" << step;
        ASSERT_EQ(bits.find_prev(q), expected_prev(q)) << "slots=" << slots << " q=" << q;
        ASSERT_EQ(bits.find_next(q), expected_next(q)) << "slots=" << slots << " q=" << q;
        ASSERT_EQ(bits.find_prev(slots - 1), expected_prev(slots - 1));
        ASSERT_EQ(bits.find_next(0), expected_next(0));
    }
}

TEST(OccupancyBitmap, MatchesModelAcrossBoundarySizes) {
    std::uint64_t seed = 1;
    for (const std::size_t slots : {1U, 2U, 63U, 64U, 65U, 4095U, 4096U, 4097U, 70'000U}) {
        fuzz_bitmap(slots, seed++);
    }
}

TEST(OccupancyBitmap, EmptyAndSingleBit) {
    OccupancyBitmap bits(10'000);
    EXPECT_EQ(bits.find_prev(9'999), OccupancyBitmap::kNpos);
    EXPECT_EQ(bits.find_next(0), OccupancyBitmap::kNpos);
    bits.set(5'000);
    EXPECT_EQ(bits.find_prev(9'999), 5'000U);
    EXPECT_EQ(bits.find_prev(5'000), 5'000U);
    EXPECT_EQ(bits.find_prev(4'999), OccupancyBitmap::kNpos);
    EXPECT_EQ(bits.find_next(0), 5'000U);
    EXPECT_EQ(bits.find_next(5'001), OccupancyBitmap::kNpos);
    EXPECT_EQ(bits.find_next(10'000), OccupancyBitmap::kNpos);  // past the end
}

// --- ArrayLevels --------------------------------------------------------------

template <class Levels>
class ArrayLevelsTest : public ::testing::Test {};
using ArrayLevelVariants =
    ::testing::Types<ArrayLevels<BestSearch::Bitmap>, ArrayLevels<BestSearch::Linear>>;
TYPED_TEST_SUITE(ArrayLevelsTest, ArrayLevelVariants);

TYPED_TEST(ArrayLevelsTest, TracksBestLevelPerSideThroughInsertAndErase) {
    TypeParam levels(BookConfig{.min_price = Price{100}, .max_price = Price{200}, .max_orders = 8});
    EXPECT_EQ(levels.best(Side::Buy), kNoLevel);
    EXPECT_EQ(levels.best(Side::Sell), kNoLevel);

    const LevelRef b1 = levels.get_or_insert(Side::Buy, Price{110});
    const LevelRef b2 = levels.get_or_insert(Side::Buy, Price{150});
    const LevelRef a1 = levels.get_or_insert(Side::Sell, Price{190});
    const LevelRef a2 = levels.get_or_insert(Side::Sell, Price{160});
    for (const LevelRef ref : {b1, b2, a1, a2}) {
        ++levels.at(ref).count;  // pretend an order rests there
    }
    EXPECT_EQ(levels.at(levels.best(Side::Buy)).price, Price{150});
    EXPECT_EQ(levels.at(levels.best(Side::Sell)).price, Price{160});
    EXPECT_EQ(levels.get_or_insert(Side::Buy, Price{150}), b2);  // existing level

    for (const LevelRef ref : {b2, a2}) {  // empty the best levels
        levels.at(ref).count = 0;
        levels.erase(ref);
    }
    EXPECT_EQ(levels.at(levels.best(Side::Buy)).price, Price{110});
    EXPECT_EQ(levels.at(levels.best(Side::Sell)).price, Price{190});

    for (const LevelRef ref : {b1, a1}) {
        levels.at(ref).count = 0;
        levels.erase(ref);
    }
    EXPECT_EQ(levels.best(Side::Buy), kNoLevel);
    EXPECT_EQ(levels.best(Side::Sell), kNoLevel);
}

TYPED_TEST(ArrayLevelsTest, BandEdgesMapToFirstAndLastSlot) {
    TypeParam levels(BookConfig{.min_price = Price{-5}, .max_price = Price{5}, .max_orders = 4});
    EXPECT_EQ(levels.get_or_insert(Side::Buy, Price{-5}), 0U);
    EXPECT_EQ(levels.get_or_insert(Side::Sell, Price{5}), 10U);
    EXPECT_EQ(levels.at(0).price, Price{-5});
    EXPECT_EQ(levels.at(10).price, Price{5});
}

TYPED_TEST(ArrayLevelsTest, RejectsBandsThatAreTooWide) {
    const auto too_wide = static_cast<std::int64_t>(kMaxBandTicks);
    EXPECT_THROW(
        TypeParam(BookConfig{.min_price = Price{0}, .max_price = Price{too_wide}, .max_orders = 1}),
        std::invalid_argument);
    EXPECT_NO_THROW(TypeParam(
        BookConfig{.min_price = Price{0}, .max_price = Price{too_wide - 1}, .max_orders = 1}));
}

}  // namespace
}  // namespace lob::engine
