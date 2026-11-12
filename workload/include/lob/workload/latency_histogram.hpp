#pragma once

// Log-linear latency histogram in the style of HdrHistogram.
//
// - Values 0..127 are recorded exactly.
// - Above that, each power-of-two range [2^k, 2^(k+1)) is split into 64 equal
//   sub-buckets, so a recorded value is known to within 1/64 (1.6%) of itself.
// - Fixed-size array (3,776 counters, ~30 KB), covering the full uint64 range;
//   record() is a few instructions and never allocates.
// - Percentiles report the *upper edge* of the bucket that holds the requested
//   rank, so they are never under-reported; max and min are tracked exactly.
//
// Units are whatever the caller records (clock ticks here); conversion to
// nanoseconds happens at reporting time.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lob::workload {

class LatencyHistogram {
public:
    static constexpr unsigned kSubBucketBits = 7;  // 128 sub-buckets
    static constexpr std::uint64_t kSubBuckets = 1U << kSubBucketBits;
    static constexpr std::uint64_t kHalf = kSubBuckets / 2;        // 64
    static constexpr unsigned kBuckets = 64 - kSubBucketBits + 1;  // 58 octaves
    static constexpr std::size_t kCounters = (kBuckets + 1) * kHalf;

    void record(std::uint64_t value) noexcept {
        ++counts_[index_of(value)];
        ++total_;
        max_ = std::max(max_, value);
        min_ = std::min(min_, value);
    }

    void merge(const LatencyHistogram& other) noexcept {
        for (std::size_t i = 0; i < kCounters; ++i) {
            counts_[i] += other.counts_[i];
        }
        total_ += other.total_;
        max_ = std::max(max_, other.max_);
        min_ = std::min(min_, other.min_);
    }

    /// Value at percentile `p` in [0, 100]: the smallest bucket upper edge such
    /// that at least p% of recorded values are <= it. 0 if empty.
    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        if (total_ == 0) {
            return 0;
        }
        if (p >= 100.0) {
            return max_;
        }
        const auto rank = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(total_))));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kCounters; ++i) {
            seen += counts_[i];
            if (seen >= rank) {
                return std::min(highest_in(i), max_);
            }
        }
        return max_;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return total_ == 0 ? 0 : min_; }

    [[nodiscard]] double mean() const noexcept {
        if (total_ == 0) {
            return 0.0;
        }
        double sum = 0.0;
        for (std::size_t i = 0; i < kCounters; ++i) {
            if (counts_[i] != 0) {
                const auto mid = (lowest_in(i) + highest_in(i)) / 2;
                sum += static_cast<double>(counts_[i]) * static_cast<double>(mid);
            }
        }
        return sum / static_cast<double>(total_);
    }

    // Exposed for tests.
    [[nodiscard]] static constexpr std::size_t index_of(std::uint64_t value) noexcept {
        const unsigned magnitude =
            value == 0 ? 0 : 63U - static_cast<unsigned>(std::countl_zero(value));
        const unsigned bucket = magnitude < kSubBucketBits ? 0 : magnitude - (kSubBucketBits - 1);
        const std::uint64_t sub = value >> bucket;  // [0,128) for bucket 0, [64,128) otherwise
        return (std::size_t{bucket} * kHalf) + static_cast<std::size_t>(sub);
    }
    [[nodiscard]] static constexpr std::uint64_t lowest_in(std::size_t index) noexcept {
        const auto [bucket, sub] = split(index);
        return sub << bucket;
    }
    [[nodiscard]] static constexpr std::uint64_t highest_in(std::size_t index) noexcept {
        const auto [bucket, sub] = split(index);
        const std::uint64_t width = std::uint64_t{1} << bucket;
        const std::uint64_t low = sub << bucket;
        // The very last bucket's upper edge is UINT64_MAX.
        return low > std::numeric_limits<std::uint64_t>::max() - (width - 1)
                   ? std::numeric_limits<std::uint64_t>::max()
                   : low + (width - 1);
    }

private:
    struct Split {
        unsigned bucket;
        std::uint64_t sub;
    };
    [[nodiscard]] static constexpr Split split(std::size_t index) noexcept {
        if (index < kSubBuckets) {
            return {.bucket = 0, .sub = index};
        }
        const auto bucket = static_cast<unsigned>(index / kHalf) - 1;
        const std::uint64_t sub = (index % kHalf) + kHalf;
        return {.bucket = bucket, .sub = sub};
    }

    std::array<std::uint64_t, kCounters> counts_{};
    std::uint64_t total_{0};
    std::uint64_t max_{0};
    std::uint64_t min_{std::numeric_limits<std::uint64_t>::max()};
};

}  // namespace lob::workload
