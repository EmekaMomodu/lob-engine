#pragma once

// The optimized engine configurations that are tested and benchmarked.

#include "lob/book_api.hpp"
#include "lob/engine/map_levels.hpp"
#include "lob/engine/matching_engine.hpp"

namespace lob {

/// Design A: price levels in a std::map (tree) backed by a block arena.
template <class Sink>
using MapEngine = engine::MatchingEngine<engine::MapLevels, Sink>;

}  // namespace lob
