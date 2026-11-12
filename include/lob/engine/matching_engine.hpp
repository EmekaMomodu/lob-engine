#pragma once

// MatchingEngine<Levels, Sink>: the optimized order book.
//
// One implementation of validation, matching and modify rules, parameterised
// on the price-level container (design A: MapLevels, design B: ArrayLevels)
// and the event sink. Both are template parameters, so every call is static
// and inlinable: no virtual dispatch anywhere on the hot path.
//
// Memory: the order pool, id map and level storage are sized from
// BookConfig::max_orders and allocated in the constructor. After that, no
// command allocates (verified by tests/alloc/no_alloc_test.cpp).
//
// Behaviour (validation order, event order, modify semantics) must match the
// reference book exactly; tests/differential checks this on millions of
// random commands.

#include "lob/commands.hpp"
#include "lob/engine/id_map.hpp"
#include "lob/engine/level.hpp"
#include "lob/engine/order_pool.hpp"
#include "lob/events.hpp"
#include "lob/snapshot.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lob::engine {

template <PriceLevels Levels, EventSink Sink>
class MatchingEngine {
public:
    MatchingEngine(const BookConfig& config, Sink& sink)
        : config_(validated(config)),
          sink_(&sink),
          orders_(config.max_orders),
          ids_(config.max_orders),
          levels_(config) {}

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) = delete;
    MatchingEngine& operator=(MatchingEngine&&) = delete;
    ~MatchingEngine() = default;

    // ----------------------------------------------------------------- commands

    void submit(const NewOrder& cmd) {
        // Validation order (must match the reference): quantity, price,
        // duplicate id, capacity.
        if (cmd.qty <= Qty{0} || cmd.qty > kMaxOrderQty) {
            return reject(cmd.id, RejectReason::InvalidQuantity);
        }
        const bool is_limit = cmd.type == OrderType::Limit;
        if (is_limit && !in_band(cmd.price)) {
            return reject(cmd.id, RejectReason::PriceOutOfBand);
        }
        const IdMap::Probe probe = ids_.probe(cmd.id, key_of());
        if (probe.found) {
            return reject(cmd.id, RejectReason::DuplicateOrderId);
        }
        if (is_limit && orders_.live() >= config_.max_orders) {
            return reject(cmd.id, RejectReason::CapacityExceeded);
        }

        sink_->on(Accepted{.id = cmd.id});
        const auto qty = static_cast<std::uint32_t>(cmd.qty.value());
        Aggressor aggressor{.id = cmd.id, .side = cmd.side, .leaves = qty};

        if (!is_limit) {
            // A market order is a limit order at the most aggressive price.
            match(aggressor, cmd.side == Side::Buy ? kMaxPrice : kMinPrice);
            if (aggressor.leaves > 0) {
                sink_->on(Cancelled{.id = cmd.id,
                                    .leaves = to_qty(aggressor.leaves),
                                    .reason = CancelReason::NoLiquidity});
            }
            return;
        }

        match(aggressor, cmd.price);
        if (aggressor.leaves > 0) {
            // Only orders that rest take a pool slot.
            const std::uint32_t index = orders_.allocate();
            Order& order = orders_[index];
            order.id = cmd.id;
            order.leaves = aggressor.leaves;
            order.total = qty;
            order.side = cmd.side;
            // If nothing traded, matching erased nothing from the id map, so
            // the empty slot found by the duplicate check is still valid.
            if (aggressor.leaves == qty) {
                ids_.insert_at(probe.slot, index);
            } else {
                ids_.insert(cmd.id, index, key_of());
            }
            link_back(levels_.get_or_insert(cmd.side, cmd.price), index);
        }
    }

    void cancel(const Cancel& cmd) {
        const IdMap::Probe probe = ids_.probe(cmd.id, key_of());
        if (!probe.found) {
            return reject(cmd.id, RejectReason::UnknownOrderId);
        }
        const std::uint32_t index = ids_.value_at(probe.slot);
        const Qty leaves = to_qty(orders_[index].leaves);
        remove(index, probe.slot);
        sink_->on(Cancelled{.id = cmd.id, .leaves = leaves, .reason = CancelReason::UserRequested});
    }

    void modify(const Modify& cmd) {
        // Validation order (must match the reference): unknown id, quantity, price.
        const IdMap::Probe probe = ids_.probe(cmd.id, key_of());
        if (!probe.found) {
            return reject(cmd.id, RejectReason::UnknownOrderId);
        }
        const std::uint32_t index = ids_.value_at(probe.slot);
        if (cmd.qty < Qty{0} || cmd.qty > kMaxOrderQty) {
            return reject(cmd.id, RejectReason::InvalidQuantity);
        }
        if (!in_band(cmd.price)) {
            return reject(cmd.id, RejectReason::PriceOutOfBand);
        }

        Order& order = orders_[index];
        const std::uint32_t filled = order.total - order.leaves;
        const auto new_total = static_cast<std::uint32_t>(cmd.qty.value());

        if (new_total <= filled) {
            const Qty leaves = to_qty(order.leaves);
            remove(index, probe.slot);
            sink_->on(
                Cancelled{.id = cmd.id, .leaves = leaves, .reason = CancelReason::ReducedToFilled});
            return;
        }

        Level& level = levels_.at(order.level);
        const std::uint32_t new_leaves = new_total - filled;

        // Same price and not increasing: amend in place, keep queue position.
        if (cmd.price == level.price && new_total <= order.total) {
            level.total -= order.leaves - new_leaves;
            order.leaves = new_leaves;
            order.total = new_total;
            sink_->on(Modified{.id = cmd.id,
                               .price = cmd.price,
                               .leaves = to_qty(new_leaves),
                               .kept_priority = true});
            return;
        }

        // Cancel/replace: unlink first (so it can never match itself), then
        // match as an aggressor at the new price, then rest any remainder.
        unlink(order.level, index);
        order.leaves = new_leaves;
        order.total = new_total;
        sink_->on(Modified{.id = cmd.id,
                           .price = cmd.price,
                           .leaves = to_qty(new_leaves),
                           .kept_priority = false});

        Aggressor aggressor{.id = order.id, .side = order.side, .leaves = new_leaves};
        match(aggressor, cmd.price);
        if (aggressor.leaves == 0) {
            ids_.erase(cmd.id, key_of());
            orders_.release(index);
            return;
        }
        order.leaves = aggressor.leaves;
        link_back(levels_.get_or_insert(order.side, cmd.price), index);
    }

    // ------------------------------------------------------------------ queries

    [[nodiscard]] std::optional<Price> best_bid() const { return best_price(Side::Buy); }
    [[nodiscard]] std::optional<Price> best_ask() const { return best_price(Side::Sell); }
    [[nodiscard]] std::size_t order_count() const { return orders_.live(); }

    [[nodiscard]] BookSnapshot snapshot() const {
        return BookSnapshot{.bids = view(Side::Buy), .asks = view(Side::Sell)};
    }

    /// Full structural self-check (slow; tests only). Returns a description of
    /// the first violated invariant, or nullopt if the book is consistent.
    [[nodiscard]] std::optional<std::string> check_invariants() const {
        std::optional<std::string> error;
        std::size_t orders_seen = 0;
        for (const Side side : {Side::Buy, Side::Sell}) {
            std::optional<Price> previous;
            levels_.for_each(side, [&](LevelRef ref, const Level& level) {
                if (error) {
                    return;
                }
                error = check_level(ref, level, side, previous, orders_seen);
                previous = level.price;
            });
        }
        if (!error && orders_seen != orders_.live()) {
            error = "pool live count " + std::to_string(orders_.live()) + " != orders in levels "
                    + std::to_string(orders_seen);
        }
        if (!error && ids_.size() != orders_.live()) {
            error = "id map size " + std::to_string(ids_.size()) + " != pool live count "
                    + std::to_string(orders_.live());
        }
        return error;
    }

private:
    /// The incoming side of a match. Lives on the stack: an order only takes a
    /// pool slot if it ends up resting.
    struct Aggressor {
        OrderId id;
        Side side{};
        std::uint32_t leaves{};
    };

    static constexpr Price kMaxPrice{std::numeric_limits<std::int64_t>::max()};
    static constexpr Price kMinPrice{std::numeric_limits<std::int64_t>::min()};

    static BookConfig validated(const BookConfig& config) {
        if (config.min_price > config.max_price) {
            throw std::invalid_argument("BookConfig: min_price > max_price");
        }
        if (config.max_orders == 0 || config.max_orders >= kNil) {
            throw std::invalid_argument("BookConfig: max_orders must be in [1, 2^32 - 2]");
        }
        return config;
    }

    /// The id map stores only pool indices; it reads keys back from the pool.
    [[nodiscard]] auto key_of() const noexcept {
        return [this](std::uint32_t index) noexcept { return orders_[index].id.value(); };
    }

    [[nodiscard]] static Qty to_qty(std::uint64_t qty) noexcept {
        return Qty{static_cast<std::int64_t>(qty)};
    }

    [[nodiscard]] bool in_band(Price price) const noexcept {
        return price >= config_.min_price && price <= config_.max_price;
    }

    [[nodiscard]] static bool crosses(Side aggressor_side, Price limit,
                                      Price level_price) noexcept {
        return aggressor_side == Side::Buy ? level_price <= limit : level_price >= limit;
    }

    void reject(OrderId id, RejectReason reason) {
        sink_->on(Rejected{.id = id, .reason = reason});
    }

    void match(Aggressor& aggressor, Price limit) {
        const Side book_side = opposite(aggressor.side);
        while (aggressor.leaves > 0) {
            const LevelRef ref = levels_.best(book_side);
            if (ref == kNoLevel) {
                break;
            }
            Level& level = levels_.at(ref);
            if (!crosses(aggressor.side, limit, level.price)) {
                break;
            }

            // Consume this level front to back before looking up the next one.
            while (aggressor.leaves > 0 && level.head != kNil) {
                const std::uint32_t index = level.head;
                Order& resting = orders_[index];
                const std::uint32_t fill = std::min(aggressor.leaves, resting.leaves);
                resting.leaves -= fill;
                aggressor.leaves -= fill;
                level.total -= fill;

                sink_->on(Trade{.aggressor_id = aggressor.id,
                                .resting_id = resting.id,
                                .aggressor_side = aggressor.side,
                                .price = level.price,
                                .qty = to_qty(fill)});

                if (resting.leaves == 0) {
                    const OrderId resting_id = resting.id;
                    sink_->on(Filled{.id = resting_id});
                    pop_head(level);
                    ids_.erase(resting_id, key_of());
                    orders_.release(index);
                } else {
                    sink_->on(PartiallyFilled{.id = resting.id, .leaves = to_qty(resting.leaves)});
                }

                if (aggressor.leaves == 0) {
                    sink_->on(Filled{.id = aggressor.id});
                } else {
                    sink_->on(
                        PartiallyFilled{.id = aggressor.id, .leaves = to_qty(aggressor.leaves)});
                }
            }

            if (level.count == 0) {
                levels_.erase(ref);
            }
        }
    }

    // --- intrusive list operations -------------------------------------------

    void link_back(LevelRef ref, std::uint32_t index) noexcept {
        Level& level = levels_.at(ref);
        Order& order = orders_[index];
        order.level = ref;
        order.prev = level.tail;
        order.next = kNil;
        if (level.tail != kNil) {
            orders_[level.tail].next = index;
        } else {
            level.head = index;
        }
        level.tail = index;
        ++level.count;
        level.total += order.leaves;
    }

    /// Unlink an order from its level (any position); erase the level if it empties.
    void unlink(LevelRef ref, std::uint32_t index) noexcept {
        Level& level = levels_.at(ref);
        const Order& order = orders_[index];
        if (order.prev != kNil) {
            orders_[order.prev].next = order.next;
        } else {
            level.head = order.next;
        }
        if (order.next != kNil) {
            orders_[order.next].prev = order.prev;
        } else {
            level.tail = order.prev;
        }
        --level.count;
        level.total -= order.leaves;
        if (level.count == 0) {
            levels_.erase(ref);
        }
    }

    /// Pop a fully filled head order. The caller erases an emptied level once
    /// it is done with it (the matching loop still holds a reference).
    void pop_head(Level& level) noexcept {
        const std::uint32_t index = level.head;
        const std::uint32_t next = orders_[index].next;
        level.head = next;
        if (next != kNil) {
            orders_[next].prev = kNil;
        } else {
            level.tail = kNil;
        }
        --level.count;
    }

    /// Remove a resting order entirely (cancel, or modify down to filled).
    /// `id_slot` is its id-map slot from a probe made in the same command.
    void remove(std::uint32_t index, std::size_t id_slot) noexcept {
        unlink(orders_[index].level, index);
        ids_.erase_at(id_slot, key_of());
        orders_.release(index);
    }

    // --- queries -------------------------------------------------------------

    [[nodiscard]] std::optional<Price> best_price(Side side) const {
        const LevelRef ref = levels_.best(side);
        return ref == kNoLevel ? std::nullopt : std::optional<Price>{levels_.at(ref).price};
    }

    [[nodiscard]] std::vector<LevelView> view(Side side) const {
        std::vector<LevelView> out;
        levels_.for_each(side, [&](LevelRef /*ref*/, const Level& level) {
            LevelView lv{.price = level.price, .total = to_qty(level.total), .orders = {}};
            for (std::uint32_t i = level.head; i != kNil; i = orders_[i].next) {
                lv.orders.push_back(
                    OrderView{.id = orders_[i].id, .leaves = to_qty(orders_[i].leaves)});
            }
            out.push_back(std::move(lv));
        });
        return out;
    }

    [[nodiscard]] std::optional<std::string> check_level(LevelRef ref, const Level& level,
                                                         Side side, std::optional<Price> previous,
                                                         std::size_t& orders_seen) const {
        const std::string where = "level " + std::to_string(level.price.value()) + ": ";
        if (level.side != side) {
            return where + "wrong side";
        }
        if (previous && (side == Side::Buy ? level.price >= *previous : level.price <= *previous)) {
            return where + "levels not strictly best-first";
        }
        if (level.count == 0 || level.head == kNil) {
            return where + "empty level still present";
        }
        std::uint64_t total = 0;
        std::uint32_t count = 0;
        std::uint32_t prev = kNil;
        for (std::uint32_t i = level.head; i != kNil; i = orders_[i].next) {
            const Order& order = orders_[i];
            if (order.prev != prev) {
                return where + "broken prev link at order " + std::to_string(order.id.value());
            }
            if (order.level != ref || order.side != side) {
                return where + "order " + std::to_string(order.id.value())
                       + " has wrong level/side";
            }
            if (order.leaves == 0 || order.leaves > order.total) {
                return where + "order " + std::to_string(order.id.value()) + " has bad quantities";
            }
            if (ids_.find(order.id, key_of()) != i) {
                return where + "id map does not point at order " + std::to_string(order.id.value());
            }
            total += order.leaves;
            ++count;
            prev = i;
        }
        if (level.tail != prev) {
            return where + "tail does not match last order";
        }
        if (count != level.count || total != level.total) {
            return where + "count/total aggregates out of sync";
        }
        orders_seen += count;
        return std::nullopt;
    }

    BookConfig config_;
    Sink* sink_;
    OrderPool orders_;
    IdMap ids_;
    Levels levels_;
};

}  // namespace lob::engine
