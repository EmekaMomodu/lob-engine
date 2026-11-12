#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lob::engine {

/// Two-level bitmap over N slots (one bit per price tick) that answers
/// "nearest occupied slot below / above i" in a handful of instructions.
///
///   words_   : bit k of word w   = slot 64*w + k is occupied
///   summary_ : bit k of summary s = words_[64*s + k] is non-zero
///
/// A query checks the bits left in the current word (one countl_zero /
/// countr_zero); if none, it jumps through the summary to the next non-zero
/// word. One summary bit covers 4,096 ticks, so the summary for a 1M-tick band
/// is 245 words (~2 KB) and is effectively always in L1.
///
/// Worst case (an empty stretch of the band) scans summary words linearly:
/// O(N / 4096) word reads, versus O(N) Level reads for a plain linear scan.
class OccupancyBitmap {
public:
    static constexpr std::size_t kNpos = std::numeric_limits<std::size_t>::max();

    explicit OccupancyBitmap(std::size_t slots)
        : slots_(slots), words_((slots + 63) / 64), summary_((words_.size() + 63) / 64) {}

    void set(std::size_t i) noexcept {
        assert(i < slots_);
        words_[i >> 6U] |= bit(i & 63U);
        summary_[i >> 12U] |= bit((i >> 6U) & 63U);
    }

    void clear(std::size_t i) noexcept {
        assert(i < slots_);
        std::uint64_t& word = words_[i >> 6U];
        word &= ~bit(i & 63U);
        if (word == 0) {
            summary_[i >> 12U] &= ~bit((i >> 6U) & 63U);
        }
    }

    [[nodiscard]] bool test(std::size_t i) const noexcept {
        assert(i < slots_);
        return (words_[i >> 6U] & bit(i & 63U)) != 0;
    }

    /// Highest occupied slot <= i, or kNpos.
    [[nodiscard]] std::size_t find_prev(std::size_t i) const noexcept {
        assert(i < slots_);
        const std::size_t w = i >> 6U;
        const std::size_t b = i & 63U;
        const std::uint64_t at_or_below = b == 63 ? ~std::uint64_t{0} : bit(b + 1) - 1;
        if (const std::uint64_t mask = words_[w] & at_or_below; mask != 0) {
            return (w << 6U) + highest(mask);
        }
        // Words strictly below w, via the summary.
        std::size_t s = w >> 6U;
        std::uint64_t smask = summary_[s] & (bit(w & 63U) - 1);
        for (;;) {
            if (smask != 0) {
                const std::size_t ww = (s << 6U) + highest(smask);
                return (ww << 6U) + highest(words_[ww]);
            }
            if (s == 0) {
                return kNpos;
            }
            smask = summary_[--s];
        }
    }

    /// Lowest occupied slot >= i, or kNpos.
    [[nodiscard]] std::size_t find_next(std::size_t i) const noexcept {
        if (i >= slots_) {
            return kNpos;
        }
        const std::size_t w = i >> 6U;
        const std::size_t b = i & 63U;
        if (const std::uint64_t mask = words_[w] & (~std::uint64_t{0} << b); mask != 0) {
            return (w << 6U) + lowest(mask);
        }
        // Words strictly above w, via the summary.
        std::size_t s = w >> 6U;
        const std::size_t sb = w & 63U;
        std::uint64_t smask = sb == 63 ? 0 : summary_[s] & (~std::uint64_t{0} << (sb + 1));
        for (;;) {
            if (smask != 0) {
                const std::size_t ww = (s << 6U) + lowest(smask);
                return (ww << 6U) + lowest(words_[ww]);
            }
            if (++s >= summary_.size()) {
                return kNpos;
            }
            smask = summary_[s];
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return slots_; }

private:
    [[nodiscard]] static constexpr std::uint64_t bit(std::size_t k) noexcept {
        return std::uint64_t{1} << k;
    }
    [[nodiscard]] static std::size_t highest(std::uint64_t x) noexcept {
        return 63U - static_cast<std::size_t>(std::countl_zero(x));
    }
    [[nodiscard]] static std::size_t lowest(std::uint64_t x) noexcept {
        return static_cast<std::size_t>(std::countr_zero(x));
    }

    std::size_t slots_;
    std::vector<std::uint64_t> words_;
    std::vector<std::uint64_t> summary_;
};

}  // namespace lob::engine
