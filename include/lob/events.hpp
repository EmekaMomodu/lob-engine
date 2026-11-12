#pragma once

#include "lob/types.hpp"

#include <variant>

namespace lob {

// ---------------------------------------------------------------------------
// Output events
// ---------------------------------------------------------------------------
// Every book implementation must emit exactly the same events in exactly the
// same order for the same input; that is what differential testing checks.
//
// Ordering contract for one command:
//   NewOrder : Accepted | Rejected
//              then, per match:  Trade, <resting status>, <aggressor status>
//              then, market only: Cancelled(NoLiquidity) if anything is left
//   Cancel   : Cancelled | Rejected
//   Modify   : Rejected
//            | Cancelled(ReducedToFilled)
//            | Modified, then (if priority was lost and the new price crosses)
//              the same per-match sequence as a new order
// where <status> is PartiallyFilled (leaves > 0) or Filled (leaves == 0).
//
// Events carry no sequence number: ordering is the contract, and stamping a
// sequence number is the publisher's job when it serialises the stream.

struct Accepted {
    OrderId id;

    friend bool operator==(const Accepted&, const Accepted&) = default;
};

struct Rejected {
    OrderId id;
    RejectReason reason{};

    friend bool operator==(const Rejected&, const Rejected&) = default;
};

/// One match between an incoming (aggressor) order and a resting order.
/// Always executes at the resting order's price.
struct Trade {
    OrderId aggressor_id;
    OrderId resting_id;
    Side aggressor_side{};
    Price price;
    Qty qty;

    friend bool operator==(const Trade&, const Trade&) = default;
};

struct PartiallyFilled {
    OrderId id;
    Qty leaves;  ///< quantity still open after this fill

    friend bool operator==(const PartiallyFilled&, const PartiallyFilled&) = default;
};

struct Filled {
    OrderId id;

    friend bool operator==(const Filled&, const Filled&) = default;
};

struct Cancelled {
    OrderId id;
    Qty leaves;  ///< open quantity that was cancelled
    CancelReason reason{};

    friend bool operator==(const Cancelled&, const Cancelled&) = default;
};

struct Modified {
    OrderId id;
    Price price;
    Qty leaves;
    bool kept_priority{};

    friend bool operator==(const Modified&, const Modified&) = default;
};

using Event = std::variant<Accepted, Rejected, Trade, PartiallyFilled, Filled, Cancelled, Modified>;

/// A sink receives events by a statically dispatched `on(const E&)` call: no
/// virtual call and no allocation on the hot path.
template <class S>
concept EventSink = requires(S& sink) {
    sink.on(Accepted{});
    sink.on(Rejected{});
    sink.on(Trade{});
    sink.on(PartiallyFilled{});
    sink.on(Filled{});
    sink.on(Cancelled{});
    sink.on(Modified{});
};

}  // namespace lob
