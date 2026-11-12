// Throughput: replay one pre-generated command stream through each book.
//
// Milestone 3 baseline (reference vs. design A). Milestone 5 adds the
// realistic workload model, per-operation latency histograms and design B.
//
// - The workload is generated once (outside timing) by running the generator
//   against the reference book; LOB_BENCH_OPS sets its length (default 1M).
// - Each iteration replays the whole stream into a freshly constructed book.
//   Construction and destruction are excluded from timing.
// - Books emit into CountingSink, so sink cost is near zero and identical.
// - Dispatch goes through lob::apply (a std::visit per command) for every
//   book alike.

#include "lob/book_api.hpp"
#include "lob/engines.hpp"
#include "lob/reference/reference_book.hpp"
#include "lob/sinks.hpp"
#include "lob/workload/workload.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

const lob::workload::Workload& replay_workload() {
    static const lob::workload::Workload workload = [] {
        std::size_t ops = 1'000'000;
        // NOLINTNEXTLINE(concurrency-mt-unsafe): read once during static init
        if (const char* env = std::getenv("LOB_BENCH_OPS")) {
            ops = std::stoull(env);
        }
        return lob::workload::make_workload(lob::workload::replay_config(), 42, ops);
    }();
    return workload;
}

template <class Book>
void BM_Replay(benchmark::State& state) {
    const auto& workload = replay_workload();
    lob::CountingSink sink;
    for (auto _ : state) {
        state.PauseTiming();
        sink = lob::CountingSink{};
        auto book = std::make_unique<Book>(workload.config.book, sink);
        state.ResumeTiming();

        for (const lob::Command& command : workload.commands) {
            lob::process(*book, command);
        }
        benchmark::ClobberMemory();

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    const auto ops = static_cast<std::int64_t>(workload.commands.size());
    state.SetItemsProcessed(state.iterations() * ops);
    state.counters["ns_per_op"] =
        benchmark::Counter(static_cast<double>(state.iterations() * ops),
                           benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
    state.counters["events"] = static_cast<double>(sink.events());
    state.counters["trades"] = static_cast<double>(sink.trades());
    state.counters["avg_live_orders"] = static_cast<double>(workload.avg_live_orders);
    state.counters["final_levels"] = static_cast<double>(workload.final_levels);
}

BENCHMARK_TEMPLATE(BM_Replay, lob::reference::ReferenceBook<lob::CountingSink>)
    ->Name("Replay/Reference")
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Replay, lob::MapEngine<lob::CountingSink>)
    ->Name("Replay/MapEngine")
    ->Unit(benchmark::kMillisecond);

}  // namespace
