#pragma once

// Shared fixture for the order-book behaviour tests.
//
// Every test is a typed test over `BookImplementations`, so the same suite
// specifies the behaviour of the reference book now and of the optimized
// engines (designs A and B) when they are added to the list.

#include "lob/book_api.hpp"
#include "lob/commands.hpp"
#include "lob/engines.hpp"
#include "lob/events.hpp"
#include "lob/io.hpp"  // gtest failure messages
#include "lob/reference/reference_book.hpp"
#include "lob/sinks.hpp"
#include "lob/types.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace lob::test {

using Events = std::vector<Event>;

using BookImplementations =
    ::testing::Types<reference::ReferenceBook<VectorSink>, MapEngine<VectorSink>,
                     ArrayEngine<VectorSink>, ArrayScanEngine<VectorSink>>;

inline constexpr Side kBuy = Side::Buy;
inline constexpr Side kSell = Side::Sell;

// --- expected-event builders (keep test bodies readable) --------------------
inline Event acc(std::uint64_t id) {
    return Accepted{.id = OrderId{id}};
}

inline Event rej(std::uint64_t id, RejectReason reason) {
    return Rejected{.id = OrderId{id}, .reason = reason};
}

inline Event trade(std::uint64_t aggressor, std::uint64_t resting, Side aggressor_side,
                   std::int64_t price, std::int64_t qty) {
    return Trade{.aggressor_id = OrderId{aggressor},
                 .resting_id = OrderId{resting},
                 .aggressor_side = aggressor_side,
                 .price = Price{price},
                 .qty = Qty{qty}};
}

inline Event partial(std::uint64_t id, std::int64_t leaves) {
    return PartiallyFilled{.id = OrderId{id}, .leaves = Qty{leaves}};
}

inline Event filled(std::uint64_t id) {
    return Filled{.id = OrderId{id}};
}

inline Event cancelled(std::uint64_t id, std::int64_t leaves,
                       CancelReason reason = CancelReason::UserRequested) {
    return Cancelled{.id = OrderId{id}, .leaves = Qty{leaves}, .reason = reason};
}

inline Event modified(std::uint64_t id, std::int64_t price, std::int64_t leaves,
                      bool kept_priority) {
    return Modified{.id = OrderId{id},
                    .price = Price{price},
                    .leaves = Qty{leaves},
                    .kept_priority = kept_priority};
}

// --- fixture ----------------------------------------------------------------
template <class BookT>
class BookTest : public ::testing::Test {
protected:
    static_assert(OrderBook<BookT>);

    static constexpr BookConfig kConfig{
        .min_price = Price{1}, .max_price = Price{10'000}, .max_orders = 16};

    void limit(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty) {
        book.submit(limit_order(OrderId{id}, side, Price{price}, Qty{qty}));
    }

    void market(std::uint64_t id, Side side, std::int64_t qty) {
        book.submit(market_order(OrderId{id}, side, Qty{qty}));
    }

    void cancel(std::uint64_t id) { book.cancel(Cancel{.id = OrderId{id}}); }

    void modify(std::uint64_t id, std::int64_t price, std::int64_t qty) {
        book.modify(Modify{.id = OrderId{id}, .price = Price{price}, .qty = Qty{qty}});
    }

    /// Events emitted since the last call.
    Events take() { return sink.take(); }

    /// Order ids resting at a price level, in time priority. Empty if no level.
    [[nodiscard]] std::vector<std::uint64_t> queue_at(Side side, std::int64_t price) const {
        std::vector<std::uint64_t> ids;
        if (const auto* level = find_level(side, price)) {
            for (const auto& order : level->orders) {
                ids.push_back(order.id.value());
            }
        }
        return ids;
    }

    /// Total open quantity at a price level (0 if no level).
    [[nodiscard]] std::int64_t qty_at(Side side, std::int64_t price) const {
        const auto* level = find_level(side, price);
        return level != nullptr ? level->total.value() : 0;
    }

    [[nodiscard]] std::optional<std::int64_t> best_bid() const {
        const auto px = book.best_bid();
        return px ? std::optional<std::int64_t>{px->value()} : std::nullopt;
    }

    [[nodiscard]] std::optional<std::int64_t> best_ask() const {
        const auto px = book.best_ask();
        return px ? std::optional<std::int64_t>{px->value()} : std::nullopt;
    }

    VectorSink sink;
    BookT book{kConfig, sink};

private:
    // Returns a pointer into a temporary snapshot cache; valid until next call.
    [[nodiscard]] const LevelView* find_level(Side side, std::int64_t price) const {
        snapshot_cache_ = book.snapshot();
        const auto& levels = side == Side::Buy ? snapshot_cache_.bids : snapshot_cache_.asks;
        for (const auto& level : levels) {
            if (level.price == Price{price}) {
                return &level;
            }
        }
        return nullptr;
    }

    mutable BookSnapshot snapshot_cache_;
};

}  // namespace lob::test
