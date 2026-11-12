// Isolates design B's one expensive operation: finding the next best level
// after the best one empties, as a function of the gap (in ticks) to it.
//
// Setup: 64 bid levels exactly `gap` ticks apart in a 1M-tick band.
// Each iteration empties the best level (forcing a search across `gap` empty
// ticks to the next one) and then re-creates it (restoring the state).
// So one iteration = erase + next-best search + insert.
//
// Compared: BestSearch::Bitmap (two-level occupancy bitmap) vs
// BestSearch::Linear (walk the Level array tick by tick).

#include "lob/engine/array_levels.hpp"
#include "lob/types.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>

namespace {

using lob::BookConfig;
using lob::Price;
using lob::Side;
using lob::engine::ArrayLevels;
using lob::engine::BestSearch;
using lob::engine::LevelRef;

template <BestSearch Search>
void BM_NextBestSearch(benchmark::State& state) {
    const std::int64_t gap = state.range(0);
    constexpr std::int64_t kLevels = 64;
    constexpr std::int64_t kTop = 900'000;
    ArrayLevels<Search> levels(
        BookConfig{.min_price = Price{1}, .max_price = Price{1'000'000}, .max_orders = 1024});
    for (std::int64_t i = 0; i < kLevels; ++i) {
        ++levels.at(levels.get_or_insert(Side::Buy, Price{kTop - (i * gap)})).count;
    }

    for (auto _ : state) {
        const LevelRef best = levels.best(Side::Buy);
        levels.at(best).count = 0;
        levels.erase(best);  // searches `gap` ticks down for the next level
        benchmark::DoNotOptimize(levels.best(Side::Buy));
        ++levels.at(levels.get_or_insert(Side::Buy, Price{kTop})).count;  // restore
    }
    state.counters["gap_ticks"] = static_cast<double>(gap);
}

BENCHMARK_TEMPLATE(BM_NextBestSearch, BestSearch::Bitmap)
    ->Name("NextBestSearch/Bitmap")
    ->RangeMultiplier(4)
    ->Range(1, 4096);
BENCHMARK_TEMPLATE(BM_NextBestSearch, BestSearch::Linear)
    ->Name("NextBestSearch/Linear")
    ->RangeMultiplier(4)
    ->Range(1, 4096);

}  // namespace
