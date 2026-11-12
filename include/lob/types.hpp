#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace lob {

// ---------------------------------------------------------------------------
// Strong integer types
// ---------------------------------------------------------------------------
// Price, Qty and OrderId are all 64-bit integers underneath. Making them
// distinct types turns "passed qty where price was expected" into a compile
// error instead of a silent mis-trade. The wrapper is a trivially copyable
// 8-byte value, so it costs nothing at -O1 and above.
//
// Arithmetic is opt-in per tag: adding two quantities is meaningful, adding
// two order IDs is not.
template <class Rep, class Tag>
class StrongInt {
public:
    using rep_type = Rep;

    constexpr StrongInt() noexcept = default;
    constexpr explicit StrongInt(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

    friend constexpr auto operator<=>(const StrongInt&, const StrongInt&) noexcept = default;

    friend constexpr StrongInt operator+(StrongInt lhs, StrongInt rhs) noexcept
        requires Tag::kArithmetic
    {
        return StrongInt{lhs.value_ + rhs.value_};
    }
    friend constexpr StrongInt operator-(StrongInt lhs, StrongInt rhs) noexcept
        requires Tag::kArithmetic
    {
        return StrongInt{lhs.value_ - rhs.value_};
    }
    constexpr StrongInt& operator+=(StrongInt rhs) noexcept
        requires Tag::kArithmetic
    {
        value_ += rhs.value_;
        return *this;
    }
    constexpr StrongInt& operator-=(StrongInt rhs) noexcept
        requires Tag::kArithmetic
    {
        value_ -= rhs.value_;
        return *this;
    }

private:
    Rep value_{};
};

struct PriceTag {
    static constexpr bool kArithmetic = true;
};
struct QtyTag {
    static constexpr bool kArithmetic = true;
};
struct OrderIdTag {
    static constexpr bool kArithmetic = false;
};

/// Price in integer ticks. Signed so spreads/differences are representable.
using Price = StrongInt<std::int64_t, PriceTag>;
/// Quantity in lots. Signed at the API boundary so that negative input can be
/// represented and rejected rather than wrapping to a huge unsigned value.
using Qty = StrongInt<std::int64_t, QtyTag>;
/// Client-supplied order identifier. Unique among *live* orders only.
using OrderId = StrongInt<std::uint64_t, OrderIdTag>;

/// Largest accepted order quantity. Chosen below 2^32 so optimized engines can
/// store per-order quantities in 32 bits; every engine enforces the same limit
/// so differential tests agree on what is rejected.
inline constexpr Qty kMaxOrderQty{1'000'000'000};

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------
enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,   ///< Rests if not fully filled (GTC).
    Market,  ///< Fills what it can; any remainder is cancelled (IOC).
};

enum class RejectReason : std::uint8_t {
    InvalidQuantity,   ///< qty <= 0 (new order), qty < 0 (modify), or qty > kMaxOrderQty
    PriceOutOfBand,    ///< limit price outside [min_price, max_price]
    DuplicateOrderId,  ///< id already belongs to a live (resting) order
    UnknownOrderId,    ///< cancel/modify of an id that is not live
    CapacityExceeded,  ///< book already holds max_orders resting orders
};

enum class CancelReason : std::uint8_t {
    UserRequested,    ///< explicit cancel
    NoLiquidity,      ///< unfilled remainder of a market order
    ReducedToFilled,  ///< modify set total qty <= already-filled qty
};

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr std::string_view to_string(Side side) noexcept {
    return side == Side::Buy ? "Buy" : "Sell";
}

[[nodiscard]] constexpr std::string_view to_string(OrderType type) noexcept {
    return type == OrderType::Limit ? "Limit" : "Market";
}

[[nodiscard]] constexpr std::string_view to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::InvalidQuantity:
            return "InvalidQuantity";
        case RejectReason::PriceOutOfBand:
            return "PriceOutOfBand";
        case RejectReason::DuplicateOrderId:
            return "DuplicateOrderId";
        case RejectReason::UnknownOrderId:
            return "UnknownOrderId";
        case RejectReason::CapacityExceeded:
            return "CapacityExceeded";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(CancelReason reason) noexcept {
    switch (reason) {
        case CancelReason::UserRequested:
            return "UserRequested";
        case CancelReason::NoLiquidity:
            return "NoLiquidity";
        case CancelReason::ReducedToFilled:
            return "ReducedToFilled";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
/// Shared by every book implementation so they agree on what is valid.
struct BookConfig {
    Price min_price{1};         ///< inclusive lower bound of the price band (ticks)
    Price max_price{1};         ///< inclusive upper bound of the price band (ticks)
    std::size_t max_orders{1};  ///< maximum number of simultaneously resting orders
};

}  // namespace lob

template <class Rep, class Tag>
struct std::hash<lob::StrongInt<Rep, Tag>> {
    std::size_t operator()(const lob::StrongInt<Rep, Tag>& value) const noexcept {
        return std::hash<Rep>{}(value.value());
    }
};
