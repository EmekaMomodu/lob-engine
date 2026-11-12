#pragma once

#include "lob/events.hpp"

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

}  // namespace lob
