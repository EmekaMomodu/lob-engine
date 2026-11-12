#pragma once

// Human-readable printing for types, commands, events and snapshots.
// Used by test failure messages and replay tools; never on the hot path.

#include "lob/commands.hpp"
#include "lob/events.hpp"
#include "lob/snapshot.hpp"
#include "lob/types.hpp"

#include <ostream>

namespace lob {

template <class Rep, class Tag>
std::ostream& operator<<(std::ostream& os, const StrongInt<Rep, Tag>& value) {
    return os << value.value();
}

inline std::ostream& operator<<(std::ostream& os, Side side) {
    return os << to_string(side);
}
inline std::ostream& operator<<(std::ostream& os, OrderType type) {
    return os << to_string(type);
}
inline std::ostream& operator<<(std::ostream& os, RejectReason reason) {
    return os << to_string(reason);
}
inline std::ostream& operator<<(std::ostream& os, CancelReason reason) {
    return os << to_string(reason);
}

// --- commands ---------------------------------------------------------------
inline std::ostream& operator<<(std::ostream& os, const NewOrder& cmd) {
    os << "NewOrder{id=" << cmd.id << ' ' << cmd.side << ' ' << cmd.type;
    if (cmd.type == OrderType::Limit) {
        os << " px=" << cmd.price;
    }
    return os << " qty=" << cmd.qty << '}';
}
inline std::ostream& operator<<(std::ostream& os, const Cancel& cmd) {
    return os << "Cancel{id=" << cmd.id << '}';
}
inline std::ostream& operator<<(std::ostream& os, const Modify& cmd) {
    return os << "Modify{id=" << cmd.id << " px=" << cmd.price << " qty=" << cmd.qty << '}';
}

// --- events -----------------------------------------------------------------
inline std::ostream& operator<<(std::ostream& os, const Accepted& e) {
    return os << "Accepted{id=" << e.id << '}';
}
inline std::ostream& operator<<(std::ostream& os, const Rejected& e) {
    return os << "Rejected{id=" << e.id << ' ' << e.reason << '}';
}
inline std::ostream& operator<<(std::ostream& os, const Trade& e) {
    return os << "Trade{aggressor=" << e.aggressor_id << " resting=" << e.resting_id << ' '
              << e.aggressor_side << " px=" << e.price << " qty=" << e.qty << '}';
}
inline std::ostream& operator<<(std::ostream& os, const PartiallyFilled& e) {
    return os << "PartiallyFilled{id=" << e.id << " leaves=" << e.leaves << '}';
}
inline std::ostream& operator<<(std::ostream& os, const Filled& e) {
    return os << "Filled{id=" << e.id << '}';
}
inline std::ostream& operator<<(std::ostream& os, const Cancelled& e) {
    return os << "Cancelled{id=" << e.id << " leaves=" << e.leaves << ' ' << e.reason << '}';
}
inline std::ostream& operator<<(std::ostream& os, const Modified& e) {
    return os << "Modified{id=" << e.id << " px=" << e.price << " leaves=" << e.leaves
              << (e.kept_priority ? " kept-priority" : " lost-priority") << '}';
}

// --- snapshot ---------------------------------------------------------------
inline std::ostream& operator<<(std::ostream& os, const OrderView& o) {
    return os << o.id << ':' << o.leaves;
}
inline std::ostream& operator<<(std::ostream& os, const LevelView& level) {
    os << level.price << " x" << level.total << " [";
    const char* sep = "";
    for (const auto& order : level.orders) {
        os << sep << order;
        sep = " ";
    }
    return os << ']';
}
inline std::ostream& operator<<(std::ostream& os, const BookSnapshot& snap) {
    os << "bids:";
    for (const auto& level : snap.bids) {
        os << " {" << level << '}';
    }
    os << " | asks:";
    for (const auto& level : snap.asks) {
        os << " {" << level << '}';
    }
    return os;
}

}  // namespace lob
