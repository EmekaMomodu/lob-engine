// Throughput: replay one pre-generated command stream through each book.
//
// Books: the reference, design A (MapEngine), design B (ArrayEngine, bitmap
// best-level search) and design B with a linear best-level scan
// (ArrayScanEngine). Workload shapes:
//   tight  - prices cluster within ~25 ticks of the mid; dense levels
//   sparse - orders spread over ~5,000 ticks of a 1M-tick band; wide gaps
//
// - Workloads are generated once (outside timing) by running the generator
//   against the reference book; LOB_BENCH_OPS sets their length (default 1M).
// - Each iteration replays the whole stream into a freshly constructed book.
//   Construction and destruction are excluded from timing.
// - Books emit into CountingSink, so sink cost is near zero and identical.
// - Dispatch goes through lob::process (a std::visit per command) for every
//   book alike.
//
// Milestone 5 adds per-operation latency histograms on top of this.

#include "lob/book_api.hpp"
#include "lob/engines.hpp"
#include "lob/reference/reference_book.hpp"
#include "lob/sinks.hpp"
#include "lob/workload/workload.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

enum class Shape : std::uint8_t { Tight, Sparse };

std::size_t workload_length() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read during single-threaded setup
    if (const char* env = std::getenv("LOB_BENCH_OPS")) {
        return std::stoull(env);
    }
    return 1'000'000;
}

const lob::workload::Workload& workload(Shape shape) {
    static const lob::workload::Workload tight =
        lob::workload::make_workload(lob::workload::replay_config(), 42, workload_length());
    static const lob::workload::Workload sparse =
        lob::workload::make_workload(lob::workload::replay_sparse_config(), 42, workload_length());
    return shape == Shape::Tight ? tight : sparse;
}

template <class Book>
void BM_Replay(benchmark::State& state, Shape shape) {
    const auto& w = workload(shape);
    lob::CountingSink sink;
    for (auto _ : state) {
        state.PauseTiming();
        sink = lob::CountingSink{};
        auto book = std::make_unique<Book>(w.config.book, sink);
        state.ResumeTiming();

        for (const lob::Command& command : w.commands) {
            lob::process(*book, command);
        }
        benchmark::ClobberMemory();

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    const auto ops = static_cast<std::int64_t>(w.commands.size());
    state.SetItemsProcessed(state.iterations() * ops);
    state.counters["ns_per_op"] =
        benchmark::Counter(static_cast<double>(state.iterations() * ops),
                           benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
    state.counters["events"] = static_cast<double>(sink.events());
    state.counters["trades"] = static_cast<double>(sink.trades());
    state.counters["avg_live_orders"] = static_cast<double>(w.avg_live_orders);
    state.counters["final_levels"] = static_cast<double>(w.final_levels);
}

using Reference = lob::reference::ReferenceBook<lob::CountingSink>;
using MapEngine = lob::MapEngine<lob::CountingSink>;
using ArrayEngine = lob::ArrayEngine<lob::CountingSink>;
using ArrayScanEngine = lob::ArrayScanEngine<lob::CountingSink>;

BENCHMARK_CAPTURE(BM_Replay<Reference>, tight, Shape::Tight)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Replay<MapEngine>, tight, Shape::Tight)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Replay<ArrayEngine>, tight, Shape::Tight)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Replay<ArrayScanEngine>, tight, Shape::Tight)->Unit(benchmark::kMillisecond);

BENCHMARK_CAPTURE(BM_Replay<Reference>, sparse, Shape::Sparse)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Replay<MapEngine>, sparse, Shape::Sparse)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Replay<ArrayEngine>, sparse, Shape::Sparse)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Replay<ArrayScanEngine>, sparse, Shape::Sparse)->Unit(benchmark::kMillisecond);

}  // namespace
