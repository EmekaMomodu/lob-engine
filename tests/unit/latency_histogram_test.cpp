// LatencyHistogram: bucketing maths and percentile accuracy vs. exact values.

#include "lob/workload/latency_histogram.hpp"

#include "lob/workload/rng.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace lob::workload {
namespace {

using H = LatencyHistogram;

TEST(LatencyHistogram, SmallValuesAreExact) {
    for (std::uint64_t v = 0; v < 128; ++v) {
        EXPECT_EQ(H::index_of(v), v);
        EXPECT_EQ(H::lowest_in(H::index_of(v)), v);
        EXPECT_EQ(H::highest_in(H::index_of(v)), v);
    }
}

TEST(LatencyHistogram, BucketsAreContiguousAndContainTheirValues) {
    std::size_t previous = 0;
    for (std::uint64_t v = 1; v < (1U << 20U); v += 1 + (v / 97)) {
        const std::size_t i = H::index_of(v);
        EXPECT_GE(i, previous) << "index must be monotonic in value";
        EXPECT_LE(H::lowest_in(i), v);
        EXPECT_GE(H::highest_in(i), v);
        // Relative width <= 1/64 above the exact range.
        if (v >= 128) {
            EXPECT_LE(static_cast<double>(H::highest_in(i) - H::lowest_in(i) + 1),
                      (static_cast<double>(H::lowest_in(i)) / 64.0) + 1.0);
        }
        previous = i;
    }
    // Extremes stay in range.
    EXPECT_LT(H::index_of(std::numeric_limits<std::uint64_t>::max()), H::kCounters);
    EXPECT_EQ(H::highest_in(H::index_of(std::numeric_limits<std::uint64_t>::max())),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(LatencyHistogram, PercentilesMatchExactWithinBucketError) {
    Rng rng(9);
    H histogram;
    std::vector<std::uint64_t> values;
    for (int i = 0; i < 200'000; ++i) {
        // Log-uniform-ish over ~1..10^7 plus a heavy tail.
        const auto magnitude = rng.below(24);
        const std::uint64_t v =
            rng.below(std::uint64_t{1} << magnitude) + (rng.per_mille(1) ? 50'000'000 : 0);
        histogram.record(v);
        values.push_back(v);
    }
    std::ranges::sort(values);
    for (const double p : {0.0, 1.0, 50.0, 90.0, 99.0, 99.9, 99.99}) {
        const auto rank = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(values.size()))));
        const std::uint64_t exact = values[rank - 1];
        const std::uint64_t reported = histogram.percentile(p);
        EXPECT_GE(reported, exact) << "p" << p << " must never under-report";
        EXPECT_LE(static_cast<double>(reported),
                  (static_cast<double>(exact) * (1.0 + (1.0 / 64.0))) + 1.0)
            << "p" << p;
    }
    EXPECT_EQ(histogram.percentile(100.0), values.back());
    EXPECT_EQ(histogram.max(), values.back());
    EXPECT_EQ(histogram.min(), values.front());
    EXPECT_EQ(histogram.count(), values.size());
}

TEST(LatencyHistogram, MergeEqualsRecordingEverything) {
    H a;
    H b;
    H all;
    for (std::uint64_t v = 0; v < 5'000; ++v) {
        (v % 3 == 0 ? a : b).record(v * 7);
        all.record(v * 7);
    }
    a.merge(b);
    for (const double p : {10.0, 50.0, 99.0, 100.0}) {
        EXPECT_EQ(a.percentile(p), all.percentile(p));
    }
    EXPECT_EQ(a.count(), all.count());
}

TEST(LatencyHistogram, EmptyHistogramIsZero) {
    const H h;
    EXPECT_EQ(h.percentile(50.0), 0U);
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.min(), 0U);
}

}  // namespace
}  // namespace lob::workload
