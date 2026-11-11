// Placeholder until Milestone 5: proves Google Benchmark is wired up and
// that the smoke-test target runs in CI. Replaced by real book benchmarks.

#include <benchmark/benchmark.h>

#include <cstdint>

namespace {

void BM_Baseline_IncrementCounter(benchmark::State& state) {
    std::uint64_t counter = 0;
    for (auto _ : state) {
        ++counter;
        benchmark::DoNotOptimize(counter);
    }
}
BENCHMARK(BM_Baseline_IncrementCounter);

}  // namespace
