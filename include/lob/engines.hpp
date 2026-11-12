#pragma once

// The optimized engine configurations that are tested and benchmarked. All
// share one MatchingEngine; only the price-level container differs.

#include "lob/book_api.hpp"
#include "lob/engine/array_levels.hpp"
#include "lob/engine/map_levels.hpp"
#include "lob/engine/matching_engine.hpp"

namespace lob {

/// Design A: price levels in a std::map (tree) backed by a block arena.
template <class Sink>
using MapEngine = engine::MatchingEngine<engine::MapLevels, Sink>;

/// Design B: flat array of levels indexed by tick, best price found through a
/// two-level occupancy bitmap.
template <class Sink>
using ArrayEngine = engine::MatchingEngine<engine::ArrayLevels<engine::BestSearch::Bitmap>, Sink>;

/// Design B variant: same array, but the next best level is found by a linear
/// scan of the array. Exists to measure what the bitmap buys.
template <class Sink>
using ArrayScanEngine =
    engine::MatchingEngine<engine::ArrayLevels<engine::BestSearch::Linear>, Sink>;

}  // namespace lob
