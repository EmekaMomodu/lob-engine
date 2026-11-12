#pragma once

// Random command generator for differential testing (and, with a realistic
// configuration, benchmark workloads).
//
// Deterministic for a given (config, seed) on every platform: it uses its own
// RNG and integer-only distributions, and never makes a decision based on
// unordered-container iteration order.
//
// It tracks which orders are live by observing the reference book's events
// (Accepted adds an id, Filled / Cancelled remove it), so cancels and
// modifies mostly target real resting orders, while a configurable fraction
// deliberately target unknown ids, reuse ids, or carry invalid input.

#include "lob/commands.hpp"
#include "lob/events.hpp"
#include "lob/types.hpp"
#include "lob/workload/rng.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lob::workload {

struct GeneratorConfig {
    std::string name{"default"};
    BookConfig book{.min_price = Price{1}, .max_price = Price{100'000}, .max_orders = 4096};

    // Relative command weights.
    std::uint32_t w_limit{50};
    std::uint32_t w_market{5};
    std::uint32_t w_cancel{30};
    std::uint32_t w_modify{15};

    // Price model (ticks). Passive limits sit 1..max_passive_offset behind the
    // mid; aggressive_pct of limits are priced 0..max_aggressive_offset through it.
    std::int64_t max_passive_offset{20};
    // Passive offset distribution. 0: uniform in [1, max_passive_offset].
    // p > 0: geometric -- each extra tick behind the mid has probability p%,
    // so mean offset ~ 1 / (1 - p/100) and most orders sit near the touch,
    // as in real books. (Capped at max_passive_offset.)
    std::uint32_t passive_decay_pct{0};
    std::uint32_t aggressive_pct{10};
    std::int64_t max_aggressive_offset{5};
    std::uint32_t mid_move_pct{5};  // chance per command that the mid moves one tick

    // Quantities are uniform in [1, max_qty].
    std::int64_t max_qty{100};

    // Edge-case injection rates, per mille of the relevant command type.
    std::uint32_t invalid_qty_pm{5};
    std::uint32_t out_of_band_pm{5};
    std::uint32_t band_edge_pm{5};
    std::uint32_t duplicate_id_pm{5};
    std::uint32_t unknown_id_pm{10};
    std::uint32_t id_reuse_pm{20};

    // Cancels/modifies pick among the most recently added live orders this
    // often (real flow cancels recent orders far more than old ones).
    std::uint32_t recent_target_pm{500};

    // Steady-state depth. If non-zero, the cancel weight is scaled by
    // live / target_live_orders: each resting order then has a constant cancel
    // hazard, so total cancel flow is proportional to book depth and the book
    // settles near the target size (an M/M/inf-style queue) instead of draining
    // or growing without bound. 0 = fixed cancel weight.
    std::size_t target_live_orders{0};
};

class Generator {
public:
    Generator(GeneratorConfig config, std::uint64_t seed)
        : config_(std::move(config)),
          rng_(seed),
          mid_((config_.book.min_price.value() + config_.book.max_price.value()) / 2) {}

    [[nodiscard]] Command next() {
        if (rng_.percent(config_.mid_move_pct)) {
            mid_ = std::clamp(mid_ + (rng_.coin() ? 1 : -1), config_.book.min_price.value(),
                              config_.book.max_price.value());
        }

        std::uint64_t w_cancel = config_.w_cancel;
        if (config_.target_live_orders > 0) {
            w_cancel = w_cancel * live_ids_.size() / config_.target_live_orders;
        }
        const std::uint64_t total =
            std::uint64_t{config_.w_limit} + config_.w_market + w_cancel + config_.w_modify;
        std::uint64_t roll = rng_.below(total);

        Command command = [&]() -> Command {
            if (roll < config_.w_limit) {
                return make_limit();
            }
            roll -= config_.w_limit;
            if (roll < config_.w_market) {
                return make_market();
            }
            roll -= config_.w_market;
            if (live_.empty()) {
                return make_limit();
            }
            if (roll < w_cancel) {
                return Cancel{.id = target_id()};
            }
            return make_modify();
        }();
        last_ = command;
        return command;
    }

    /// Feed every event the reference book emitted for the last command.
    void observe(const Event& event) {
        if (const auto* accepted = std::get_if<Accepted>(&event)) {
            if (const auto* order = last_ ? std::get_if<NewOrder>(&*last_) : nullptr;
                order != nullptr && order->id == accepted->id) {
                add_live(
                    order->id.value(),
                    LiveOrder{.side = order->side, .price = order->price, .total = order->qty});
            }
        } else if (const auto* f = std::get_if<Filled>(&event)) {
            remove_live(f->id.value());
        } else if (const auto* c = std::get_if<Cancelled>(&event)) {
            remove_live(c->id.value());
        } else if (const auto* m = std::get_if<Modified>(&event)) {
            const auto it = live_.find(m->id.value());
            const auto* modify = last_ ? std::get_if<Modify>(&*last_) : nullptr;
            if (it != live_.end() && modify != nullptr) {
                it->second.order.price = modify->price;
                it->second.order.total = modify->qty;
            }
        }
    }

    [[nodiscard]] std::size_t live_count() const noexcept { return live_.size(); }
    [[nodiscard]] const GeneratorConfig& config() const noexcept { return config_; }

private:
    struct LiveOrder {
        Side side{};
        Price price;
        Qty total;
    };
    struct LiveEntry {
        std::size_t position{};  // index into live_ids_
        LiveOrder order;
    };

    // --- commands ------------------------------------------------------------

    // NOTE: every RNG draw is its own statement. C++ leaves the evaluation
    // order of function arguments (and of operands like `a() * b()`)
    // unspecified, so `f(rng.x(), rng.y())` can consume the stream in a
    // different order under GCC and Clang and break cross-platform
    // reproducibility. GeneratorGoldenSequence pins the output.

    NewOrder make_limit() {
        const Side side = rng_.coin() ? Side::Buy : Side::Sell;
        const OrderId id{new_order_id()};
        const Price price = limit_price(side);
        const Qty qty = quantity();
        return limit_order(id, side, price, qty);
    }

    NewOrder make_market() {
        const Side side = rng_.coin() ? Side::Buy : Side::Sell;
        const OrderId id{new_order_id()};
        const Qty qty = quantity();
        return market_order(id, side, qty);
    }

    Modify make_modify() {
        const OrderId id = target_id();
        const auto it = live_.find(id.value());
        const LiveOrder order =
            it != live_.end()
                ? it->second.order
                : LiveOrder{.side = Side::Buy, .price = Price{mid_}, .total = Qty{config_.max_qty}};
        const Price price = modify_price(order);
        const Qty qty = modify_quantity(order.total.value());
        return Modify{.id = id, .price = price, .qty = qty};
    }

    Price modify_price(const LiveOrder& order) {
        if (rng_.per_mille(config_.out_of_band_pm)) {
            return out_of_band_price();
        }
        const auto r = rng_.below(100);
        if (r < 60) {
            return order.price;  // quantity-only modify
        }
        if (r < 85) {
            const std::int64_t sign = rng_.coin() ? 1 : -1;
            const std::int64_t step = rng_.between(1, 3);
            return clamp_to_band(order.price.value() + (sign * step));
        }
        return aggressive_price(order.side);
    }

    Qty modify_quantity(std::int64_t total) {
        const auto r = rng_.below(100);
        if (r < 30) {
            return Qty{total > 1 ? rng_.between(1, total - 1) : total};  // decrease
        }
        if (r < 55) {
            return Qty{total + rng_.between(1, config_.max_qty)};  // increase
        }
        if (r < 75) {
            return Qty{total};  // unchanged
        }
        if (r < 83) {
            return Qty{0};  // reduce to nothing
        }
        if (r < 98) {
            return Qty{rng_.between(1, 3)};  // small: often at or below what has filled
        }
        return Qty{-rng_.between(1, 5)};  // invalid
    }

    // --- fields --------------------------------------------------------------

    std::uint64_t new_order_id() {
        if (!live_ids_.empty() && rng_.per_mille(config_.duplicate_id_pm)) {
            return live_ids_[rng_.below(live_ids_.size())];  // duplicate of a live order
        }
        if (!dead_ids_.empty() && rng_.per_mille(config_.id_reuse_pm)) {
            return dead_ids_[rng_.below(dead_ids_.size())];  // reuse after fill/cancel
        }
        return next_id_++;
    }

    OrderId target_id() {
        if (live_ids_.empty() || rng_.per_mille(config_.unknown_id_pm)) {
            return OrderId{next_unknown_id_++};
        }
        const std::size_t n = live_ids_.size();
        if (rng_.per_mille(config_.recent_target_pm)) {
            const std::size_t window = std::min<std::size_t>(n, 16);
            return OrderId{live_ids_[n - 1 - rng_.below(window)]};
        }
        return OrderId{live_ids_[rng_.below(n)]};
    }

    Qty quantity() {
        if (rng_.per_mille(config_.invalid_qty_pm)) {
            switch (rng_.below(4)) {
                case 0:
                    return Qty{0};
                case 1:
                    return Qty{-1};
                case 2:
                    return Qty{-config_.max_qty};
                default:
                    return kMaxOrderQty + Qty{1};
            }
        }
        return Qty{rng_.between(1, config_.max_qty)};
    }

    Price limit_price(Side side) {
        if (rng_.per_mille(config_.out_of_band_pm)) {
            return out_of_band_price();
        }
        if (rng_.per_mille(config_.band_edge_pm)) {
            return rng_.coin() ? config_.book.min_price : config_.book.max_price;
        }
        if (rng_.percent(config_.aggressive_pct)) {
            return aggressive_price(side);
        }
        const std::int64_t offset = passive_offset();
        return clamp_to_band(side == Side::Buy ? mid_ - offset : mid_ + offset);
    }

    std::int64_t passive_offset() {
        if (config_.passive_decay_pct == 0) {
            return rng_.between(1, config_.max_passive_offset);
        }
        std::int64_t offset = 1;
        while (offset < config_.max_passive_offset && rng_.percent(config_.passive_decay_pct)) {
            ++offset;
        }
        return offset;
    }

    Price aggressive_price(Side side) {
        const std::int64_t offset = rng_.between(0, config_.max_aggressive_offset);
        return clamp_to_band(side == Side::Buy ? mid_ + offset : mid_ - offset);
    }

    Price out_of_band_price() {
        const std::int64_t delta = rng_.between(1, 10);
        return rng_.coin() ? Price{config_.book.min_price.value() - delta}
                           : Price{config_.book.max_price.value() + delta};
    }

    [[nodiscard]] Price clamp_to_band(std::int64_t price) const {
        return Price{
            std::clamp(price, config_.book.min_price.value(), config_.book.max_price.value())};
    }

    // --- live-order tracking -------------------------------------------------

    void add_live(std::uint64_t id, const LiveOrder& order) {
        if (live_.contains(id)) {
            return;
        }
        live_.emplace(id, LiveEntry{.position = live_ids_.size(), .order = order});
        live_ids_.push_back(id);
    }

    void remove_live(std::uint64_t id) {
        const auto it = live_.find(id);
        if (it == live_.end()) {
            return;
        }
        // Swap-and-pop keeps removal O(1).
        const std::size_t position = it->second.position;
        const std::uint64_t moved = live_ids_.back();
        live_ids_[position] = moved;
        live_.at(moved).position = position;
        live_ids_.pop_back();
        live_.erase(id);

        if (dead_ids_.size() < kDeadIdsKept) {
            dead_ids_.push_back(id);
        } else {
            dead_ids_[rng_.below(kDeadIdsKept)] = id;
        }
    }

    static constexpr std::size_t kDeadIdsKept = 64;

    GeneratorConfig config_;
    Rng rng_;
    std::int64_t mid_;
    std::uint64_t next_id_{1};
    std::uint64_t next_unknown_id_{std::uint64_t{1} << 62U};  // never issued as a real id
    std::vector<std::uint64_t> live_ids_;                     // insertion order (approx.)
    std::unordered_map<std::uint64_t, LiveEntry> live_;
    std::vector<std::uint64_t> dead_ids_;
    std::optional<Command> last_;
};

// ---------------------------------------------------------------------------
// Named configurations
// ---------------------------------------------------------------------------

/// Narrow band and tiny capacity: constant level collisions, sweeps, band
/// edges and CapacityExceeded.
inline GeneratorConfig dense_config() {
    GeneratorConfig c;
    c.name = "dense";
    c.book = BookConfig{.min_price = Price{1}, .max_price = Price{64}, .max_orders = 64};
    c.max_passive_offset = 8;
    c.aggressive_pct = 30;
    c.max_aggressive_offset = 4;
    c.mid_move_pct = 10;
    c.max_qty = 20;
    return c;
}

/// Moderate everything; the baseline mix.
inline GeneratorConfig default_config() {
    return GeneratorConfig{};
}

/// Mostly invalid / unusual input against a very small book.
inline GeneratorConfig edge_config() {
    GeneratorConfig c;
    c.name = "edge";
    c.book = BookConfig{.min_price = Price{1}, .max_price = Price{16}, .max_orders = 8};
    c.w_limit = 40;
    c.w_market = 10;
    c.w_cancel = 20;
    c.w_modify = 30;
    c.max_passive_offset = 4;
    c.aggressive_pct = 25;
    c.max_aggressive_offset = 3;
    c.max_qty = 10;
    c.invalid_qty_pm = 100;
    c.out_of_band_pm = 100;
    c.band_edge_pm = 100;
    c.duplicate_id_pm = 100;
    c.unknown_id_pm = 100;
    c.id_reuse_pm = 200;
    return c;
}

/// Wide band, large capacity, add-heavy: deep books and long sweeps.
inline GeneratorConfig deep_config() {
    GeneratorConfig c;
    c.name = "deep";
    c.book = BookConfig{.min_price = Price{1}, .max_price = Price{1'000'000}, .max_orders = 50'000};
    c.w_limit = 70;
    c.w_market = 8;
    c.w_cancel = 12;
    c.w_modify = 10;
    c.max_passive_offset = 500;
    c.aggressive_pct = 5;
    c.max_aggressive_offset = 50;
    c.max_qty = 1'000;
    return c;
}

inline std::vector<GeneratorConfig> named_configs() {
    return {dense_config(), default_config(), edge_config(), deep_config()};
}

inline std::optional<GeneratorConfig> find_config(const std::string& name) {
    for (auto& config : named_configs()) {
        if (config.name == name) {
            return config;
        }
    }
    return std::nullopt;
}

}  // namespace lob::workload
