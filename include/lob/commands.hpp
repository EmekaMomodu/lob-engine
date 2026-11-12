#pragma once

#include "lob/types.hpp"

#include <variant>

namespace lob {

/// A new limit or market order. `price` is ignored for market orders.
struct NewOrder {
    OrderId id;
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price;
    Qty qty;

    friend bool operator==(const NewOrder&, const NewOrder&) = default;
};

struct Cancel {
    OrderId id;

    friend bool operator==(const Cancel&, const Cancel&) = default;
};

/// Cancel/replace of a resting order.
///
/// `qty` is the new *total* order quantity, including anything already filled
/// (FIX OrderQty semantics), so leaves = qty - filled. This is unambiguous
/// under a race with a fill: a client that sends "qty 10 -> 8" never ends up
/// with more exposure than it asked for.
///
///   - same price, qty <= current total  -> in place, keeps queue priority
///   - price change or qty increase      -> loses priority (back of the queue);
///                                          a crossing new price matches immediately
///   - qty <= filled (including 0)       -> order is cancelled (ReducedToFilled)
///   - qty < 0                           -> rejected (InvalidQuantity)
struct Modify {
    OrderId id;
    Price price;
    Qty qty;

    friend bool operator==(const Modify&, const Modify&) = default;
};

using Command = std::variant<NewOrder, Cancel, Modify>;

[[nodiscard]] constexpr NewOrder limit_order(OrderId id, Side side, Price price, Qty qty) noexcept {
    return NewOrder{.id = id, .side = side, .type = OrderType::Limit, .price = price, .qty = qty};
}

[[nodiscard]] constexpr NewOrder market_order(OrderId id, Side side, Qty qty) noexcept {
    return NewOrder{
        .id = id, .side = side, .type = OrderType::Market, .price = Price{}, .qty = qty};
}

}  // namespace lob
