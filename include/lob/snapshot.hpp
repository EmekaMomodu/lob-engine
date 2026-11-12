#pragma once

#include "lob/types.hpp"

#include <vector>

namespace lob {

// Full, order-level view of a book. Used by tests to compare implementations
// state-for-state, not just event-for-event. Allocates: never on the hot path.

struct OrderView {
    OrderId id;
    Qty leaves;

    friend bool operator==(const OrderView&, const OrderView&) = default;
};

struct LevelView {
    Price price;
    Qty total;                      ///< sum of leaves at this level
    std::vector<OrderView> orders;  ///< in time priority (front first)

    friend bool operator==(const LevelView&, const LevelView&) = default;
};

struct BookSnapshot {
    std::vector<LevelView> bids;  ///< best (highest) first
    std::vector<LevelView> asks;  ///< best (lowest) first

    friend bool operator==(const BookSnapshot&, const BookSnapshot&) = default;
};

}  // namespace lob
