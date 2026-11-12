#pragma once

// ReferenceBook: the test oracle.
//
// Deliberately simple and obviously correct. It is optimised for being easy
// to check by reading, not for speed: it allocates freely, uses node-based
// standard containers, and cancels in O(orders at that level).
//
// It shares no logic with the optimized engines -- only the input, event and
// snapshot types -- so that a bug in one is not silently reproduced in the
// other. Differential tests assert both produce identical events and state.

#include "lob/book_api.hpp"
#include "lob/commands.hpp"
#include "lob/events.hpp"
#include "lob/snapshot.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lob::reference {

template <EventSink Sink>
class ReferenceBook {
public:
    ReferenceBook(const BookConfig& config, Sink& sink) : config_(config), sink_(&sink) {
        if (config.min_price > config.max_price) {
            throw std::invalid_argument("BookConfig: min_price > max_price");
        }
        if (config.max_orders == 0) {
            throw std::invalid_argument("BookConfig: max_orders must be > 0");
        }
    }

    // ----------------------------------------------------------------- commands

    void submit(const NewOrder& cmd) {
        if (const auto reason = validate(cmd)) {
            sink_->on(Rejected{.id = cmd.id, .reason = *reason});
            return;
        }
        sink_->on(Accepted{.id = cmd.id});

        Order order{
            .id = cmd.id, .side = cmd.side, .price = cmd.price, .total = cmd.qty, .filled = Qty{0}};

        if (cmd.type == OrderType::Market) {
            match(order, std::nullopt);
            if (order.leaves() > Qty{0}) {
                sink_->on(Cancelled{
                    .id = order.id, .leaves = order.leaves(), .reason = CancelReason::NoLiquidity});
            }
            return;
        }

        match(order, order.price);
        if (order.leaves() > Qty{0}) {
            rest(order);
        }
    }

    void cancel(const Cancel& cmd) {
        const auto it = orders_.find(cmd.id);
        if (it == orders_.end()) {
            sink_->on(Rejected{.id = cmd.id, .reason = RejectReason::UnknownOrderId});
            return;
        }
        const Qty leaves = it->second.leaves();
        unlink(it->second);
        orders_.erase(it);
        sink_->on(Cancelled{.id = cmd.id, .leaves = leaves, .reason = CancelReason::UserRequested});
    }

    // Validation order: UnknownOrderId, InvalidQuantity, PriceOutOfBand.
    void modify(const Modify& cmd) {
        const auto it = orders_.find(cmd.id);
        if (it == orders_.end()) {
            sink_->on(Rejected{.id = cmd.id, .reason = RejectReason::UnknownOrderId});
            return;
        }
        if (cmd.qty < Qty{0} || cmd.qty > kMaxOrderQty) {
            sink_->on(Rejected{.id = cmd.id, .reason = RejectReason::InvalidQuantity});
            return;
        }
        if (!in_band(cmd.price)) {
            sink_->on(Rejected{.id = cmd.id, .reason = RejectReason::PriceOutOfBand});
            return;
        }

        Order& order = it->second;

        // New total at or below what has already traded: nothing left to work.
        if (cmd.qty <= order.filled) {
            const Qty leaves = order.leaves();
            unlink(order);
            orders_.erase(it);
            sink_->on(
                Cancelled{.id = cmd.id, .leaves = leaves, .reason = CancelReason::ReducedToFilled});
            return;
        }

        // Same price and not increasing: amend in place, keep queue position.
        if (cmd.price == order.price && cmd.qty <= order.total) {
            order.total = cmd.qty;
            sink_->on(Modified{.id = order.id,
                               .price = order.price,
                               .leaves = order.leaves(),
                               .kept_priority = true});
            return;
        }

        // Otherwise: cancel/replace. Pull the order out of the book first, so
        // it can never match against itself, then treat it as a new aggressor.
        Order replaced = order;
        unlink(order);
        orders_.erase(it);
        replaced.price = cmd.price;
        replaced.total = cmd.qty;
        sink_->on(Modified{.id = replaced.id,
                           .price = replaced.price,
                           .leaves = replaced.leaves(),
                           .kept_priority = false});
        match(replaced, replaced.price);
        if (replaced.leaves() > Qty{0}) {
            rest(replaced);
        }
    }

    // ------------------------------------------------------------------ queries

    [[nodiscard]] std::optional<Price> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional<Price>{bids_.begin()->first};
    }

    [[nodiscard]] std::optional<Price> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional<Price>{asks_.begin()->first};
    }

    [[nodiscard]] std::size_t order_count() const { return orders_.size(); }

    [[nodiscard]] BookSnapshot snapshot() const {
        return BookSnapshot{.bids = view(bids_), .asks = view(asks_)};
    }

private:
    struct Order {
        OrderId id;
        Side side{};
        Price price;
        Qty total;   // total order quantity, including filled
        Qty filled;  // cumulative filled quantity

        [[nodiscard]] Qty leaves() const { return total - filled; }
    };

    using Queue = std::deque<OrderId>;                         // time priority, front first
    using BidLevels = std::map<Price, Queue, std::greater<>>;  // begin() = highest bid
    using AskLevels = std::map<Price, Queue, std::less<>>;     // begin() = lowest ask

    // Validation order: InvalidQuantity, PriceOutOfBand, DuplicateOrderId,
    // CapacityExceeded. Every implementation must check in this order.
    [[nodiscard]] std::optional<RejectReason> validate(const NewOrder& cmd) const {
        if (cmd.qty <= Qty{0} || cmd.qty > kMaxOrderQty) {
            return RejectReason::InvalidQuantity;
        }
        const bool is_limit = cmd.type == OrderType::Limit;
        if (is_limit && !in_band(cmd.price)) {
            return RejectReason::PriceOutOfBand;
        }
        if (orders_.contains(cmd.id)) {
            return RejectReason::DuplicateOrderId;
        }
        // Checked up front (even if the order would fully fill) so the outcome
        // never depends on how much liquidity happens to be in the book.
        if (is_limit && orders_.size() >= config_.max_orders) {
            return RejectReason::CapacityExceeded;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool in_band(Price price) const {
        return price >= config_.min_price && price <= config_.max_price;
    }

    [[nodiscard]] static bool crosses(Side aggressor_side, Price limit, Price resting_price) {
        return aggressor_side == Side::Buy ? resting_price <= limit : resting_price >= limit;
    }

    // `limit` is empty for market orders (match at any price).
    void match(Order& aggressor, std::optional<Price> limit) {
        if (aggressor.side == Side::Buy) {
            match_against(asks_, aggressor, limit);
        } else {
            match_against(bids_, aggressor, limit);
        }
    }

    template <class Levels>
    void match_against(Levels& levels, Order& aggressor, std::optional<Price> limit) {
        while (aggressor.leaves() > Qty{0} && !levels.empty()) {
            const auto level = levels.begin();
            const Price level_price = level->first;
            if (limit.has_value() && !crosses(aggressor.side, *limit, level_price)) {
                break;
            }

            Queue& queue = level->second;
            const OrderId resting_id = queue.front();
            Order& resting = orders_.at(resting_id);

            const Qty fill = std::min(aggressor.leaves(), resting.leaves());
            resting.filled += fill;
            aggressor.filled += fill;

            sink_->on(Trade{.aggressor_id = aggressor.id,
                            .resting_id = resting_id,
                            .aggressor_side = aggressor.side,
                            .price = level_price,
                            .qty = fill});

            if (resting.leaves() == Qty{0}) {
                sink_->on(Filled{.id = resting_id});
                orders_.erase(resting_id);  // `resting` is dangling from here on
                queue.pop_front();
                if (queue.empty()) {
                    levels.erase(level);
                }
            } else {
                sink_->on(PartiallyFilled{.id = resting_id, .leaves = resting.leaves()});
            }

            if (aggressor.leaves() == Qty{0}) {
                sink_->on(Filled{.id = aggressor.id});
            } else {
                sink_->on(PartiallyFilled{.id = aggressor.id, .leaves = aggressor.leaves()});
            }
        }
    }

    void rest(const Order& order) {
        orders_.emplace(order.id, order);
        if (order.side == Side::Buy) {
            bids_[order.price].push_back(order.id);
        } else {
            asks_[order.price].push_back(order.id);
        }
    }

    void unlink(const Order& order) {
        if (order.side == Side::Buy) {
            unlink_from(bids_, order);
        } else {
            unlink_from(asks_, order);
        }
    }

    template <class Levels>
    static void unlink_from(Levels& levels, const Order& order) {
        const auto level = levels.find(order.price);
        Queue& queue = level->second;
        queue.erase(std::find(queue.begin(), queue.end(), order.id));
        if (queue.empty()) {
            levels.erase(level);
        }
    }

    template <class Levels>
    [[nodiscard]] std::vector<LevelView> view(const Levels& levels) const {
        std::vector<LevelView> out;
        out.reserve(levels.size());
        for (const auto& [price, queue] : levels) {
            LevelView level{.price = price, .total = Qty{0}, .orders = {}};
            for (const OrderId id : queue) {
                const Qty leaves = orders_.at(id).leaves();
                level.total += leaves;
                level.orders.push_back(OrderView{.id = id, .leaves = leaves});
            }
            out.push_back(std::move(level));
        }
        return out;
    }

    BookConfig config_;
    Sink* sink_;
    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<OrderId, Order> orders_;  // live (resting) orders only
};

}  // namespace lob::reference
