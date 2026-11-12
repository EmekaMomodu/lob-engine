#pragma once

#include "lob/events.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace lob {

/// Records every event in order. For tests and replay tools (allocates).
class VectorSink {
public:
    template <class E>
    void on(const E& event) {
        events_.emplace_back(event);
    }

    [[nodiscard]] const std::vector<Event>& events() const noexcept { return events_; }

    /// Returns the recorded events and starts a fresh recording.
    [[nodiscard]] std::vector<Event> take() noexcept { return std::exchange(events_, {}); }

    void clear() noexcept { events_.clear(); }

private:
    std::vector<Event> events_;
};

static_assert(EventSink<VectorSink>);

/// Counts events without storing them: never allocates. For benchmarks and
/// the no-allocation test, where a recording sink would dominate the cost.
class CountingSink {
public:
    template <class E>
    void on(const E& event) noexcept {
        ++events_;
        if constexpr (std::is_same_v<E, Trade>) {
            ++trades_;
            traded_qty_ += event.qty.value();
        }
    }

    [[nodiscard]] std::uint64_t events() const noexcept { return events_; }
    [[nodiscard]] std::uint64_t trades() const noexcept { return trades_; }
    [[nodiscard]] std::int64_t traded_qty() const noexcept { return traded_qty_; }

private:
    std::uint64_t events_{0};
    std::uint64_t trades_{0};
    std::int64_t traded_qty_{0};
};

static_assert(EventSink<CountingSink>);

}  // namespace lob
