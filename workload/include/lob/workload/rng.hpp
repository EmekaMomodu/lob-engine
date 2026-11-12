#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace lob::workload {

/// xoshiro256** seeded through splitmix64.
///
/// We do not use <random>'s distributions: std::uniform_int_distribution is
/// implementation-defined, so libc++ (macOS) and libstdc++ (Linux) produce
/// different sequences from the same seed. A failing differential seed must
/// reproduce identically on every machine, so the generator and the
/// distributions are spelled out here. Integer-only: no floating point.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept {
        std::uint64_t x = seed;
        for (auto& word : state_) {
            word = splitmix64(x);
        }
    }

    std::uint64_t next() noexcept {
        const std::uint64_t result = std::rotl(state_[1] * 5, 7) * 9;
        const std::uint64_t t = state_[1] << 17U;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = std::rotl(state_[3], 45);
        return result;
    }

    /// Uniform in [0, bound). Unbiased (rejection sampling). bound > 0.
    std::uint64_t below(std::uint64_t bound) noexcept {
        const std::uint64_t threshold = (std::uint64_t{0} - bound) % bound;  // 2^64 mod bound
        for (;;) {
            const std::uint64_t r = next();
            if (r >= threshold) {
                return r % bound;
            }
        }
    }

    /// Uniform in [lo, hi] (inclusive). Requires lo <= hi and a span < 2^63.
    std::int64_t between(std::int64_t lo, std::int64_t hi) noexcept {
        const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
        return lo + static_cast<std::int64_t>(below(span));
    }

    /// True with probability per_mille / 1000.
    bool per_mille(std::uint32_t per_mille) noexcept { return below(1000) < per_mille; }

    /// True with probability pct / 100.
    bool percent(std::uint32_t pct) noexcept { return below(100) < pct; }

    bool coin() noexcept { return (next() >> 63U) != 0; }

private:
    static std::uint64_t splitmix64(std::uint64_t& x) noexcept {
        x += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = x;
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }

    std::array<std::uint64_t, 4> state_{};
};

}  // namespace lob::workload
