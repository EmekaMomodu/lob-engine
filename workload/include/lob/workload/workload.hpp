#pragma once

// Pre-generated command sequences for benchmarks and allocation tests.
//
// The generator needs feedback from a book to know which orders are live, so
// the sequence is produced by running it against the reference book once.
// Replaying the sequence later into any book is then a pure, repeatable
// workload: generation cost never pollutes a measurement.

#include "lob/book_api.hpp"
#include "lob/commands.hpp"
#include "lob/reference/reference_book.hpp"
#include "lob/sinks.hpp"
#include "lob/workload/generator.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lob::workload {

/// A benchmark-oriented mix: adds and cancels dominate, prices cluster near
/// the mid, and the book reaches a steady-state depth instead of filling up.
/// (Milestone 5 replaces this with a fuller, documented workload model.)
inline GeneratorConfig replay_config() {
    GeneratorConfig c;
    c.name = "replay";
    c.book =
        BookConfig{.min_price = Price{1}, .max_price = Price{100'000}, .max_orders = 1U << 20U};
    c.w_limit = 45;
    c.w_market = 5;
    c.w_cancel = 40;
    c.w_modify = 10;
    c.max_passive_offset = 25;
    c.aggressive_pct = 10;
    c.max_aggressive_offset = 3;
    c.max_qty = 100;
    // Cancel flow proportional to depth: the book settles around this many
    // resting orders instead of draining (see GeneratorConfig).
    c.target_live_orders = 10'000;
    // Benchmarks measure the normal path; keep invalid input rare.
    c.invalid_qty_pm = 1;
    c.out_of_band_pm = 1;
    c.band_edge_pm = 0;
    c.duplicate_id_pm = 1;
    c.unknown_id_pm = 5;
    c.id_reuse_pm = 0;
    return c;
}

/// Same flow as replay_config, but resting orders spread thinly over a wide
/// band: few orders per level and large gaps between occupied levels. This is
/// the shape where finding the next best level is expensive for a linear scan.
inline GeneratorConfig replay_sparse_config() {
    GeneratorConfig c = replay_config();
    c.name = "replay_sparse";
    c.book =
        BookConfig{.min_price = Price{1}, .max_price = Price{1'000'000}, .max_orders = 1U << 20U};
    c.max_passive_offset = 5'000;
    c.max_aggressive_offset = 20;
    c.target_live_orders = 2'000;
    return c;
}

/// M5 latency workload: the "normal day" shape.
///  - Flow is dominated by adds and cancels; cancel flow is proportional to
///    depth (constant per-order cancel hazard), so the book holds ~5,000 orders.
///  - Passive prices decay geometrically from the touch (mean ~4 ticks), so
///    most adds, and therefore most cancels, happen near the top of the book.
///  - A few aggressive limits and small market orders take liquidity.
inline GeneratorConfig realistic_config() {
    GeneratorConfig c;
    c.name = "realistic";
    c.book =
        BookConfig{.min_price = Price{1}, .max_price = Price{1'000'000}, .max_orders = 1U << 20U};
    c.w_limit = 48;
    c.w_market = 3;
    c.w_cancel = 42;
    c.w_modify = 7;
    c.target_live_orders = 5'000;
    c.max_passive_offset = 200;
    c.passive_decay_pct = 75;
    c.aggressive_pct = 6;
    c.max_aggressive_offset = 2;
    c.mid_move_pct = 2;
    c.max_qty = 100;
    c.recent_target_pm = 600;
    c.invalid_qty_pm = 1;
    c.out_of_band_pm = 1;
    c.band_edge_pm = 0;
    c.duplicate_id_pm = 1;
    c.unknown_id_pm = 5;
    c.id_reuse_pm = 0;
    return c;
}

/// M5 tail workload: a thin, gappy book hit by larger market orders. Sweeps
/// empty several levels per command and the next level is often many ticks
/// away -- the case that exposes next-best-level search cost.
inline GeneratorConfig thin_config() {
    GeneratorConfig c = realistic_config();
    c.name = "thin";
    c.w_limit = 50;
    c.w_market = 8;
    c.w_cancel = 37;
    c.w_modify = 5;
    c.target_live_orders = 400;
    c.max_passive_offset = 2'000;
    c.passive_decay_pct = 97;  // mean ~33 ticks behind the mid
    c.max_qty = 200;
    return c;
}

/// Look up any named configuration (fuzzing or benchmarking).
inline std::optional<GeneratorConfig> find_any_config(const std::string& name) {
    if (auto config = find_config(name)) {
        return config;
    }
    for (auto config :
         {replay_config(), replay_sparse_config(), realistic_config(), thin_config()}) {
        if (config.name == name) {
            return config;
        }
    }
    return std::nullopt;
}

struct Workload {
    GeneratorConfig config;
    std::uint64_t seed{};
    std::vector<Command> commands;
    std::size_t final_live_orders{};
    std::size_t avg_live_orders{};  // sampled every 1,000 commands
    std::size_t final_levels{};     // bid + ask price levels at the end
};

inline Workload make_workload(const GeneratorConfig& config, std::uint64_t seed,
                              std::size_t count) {
    Workload w{.config = config,
               .seed = seed,
               .commands = {},
               .final_live_orders = 0,
               .avg_live_orders = 0,
               .final_levels = 0};
    std::size_t live_sum = 0;
    std::size_t samples = 0;
    w.commands.reserve(count);
    VectorSink sink;
    reference::ReferenceBook<VectorSink> book(config.book, sink);
    Generator generator(config, seed);
    for (std::size_t i = 0; i < count; ++i) {
        const Command command = generator.next();
        process(book, command);
        for (const Event& event : sink.events()) {
            generator.observe(event);
        }
        sink.clear();
        w.commands.push_back(command);
        if ((i + 1) % 1000 == 0) {
            live_sum += book.order_count();
            ++samples;
        }
    }
    w.final_live_orders = book.order_count();
    w.avg_live_orders = samples > 0 ? live_sum / samples : book.order_count();
    const BookSnapshot snapshot = book.snapshot();
    w.final_levels = snapshot.bids.size() + snapshot.asks.size();
    return w;
}

}  // namespace lob::workload
