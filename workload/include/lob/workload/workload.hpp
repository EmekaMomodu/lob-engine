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
