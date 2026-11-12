// Strong types, config validation and printing helpers.

#include "lob/types.hpp"

#include "lob/commands.hpp"
#include "lob/io.hpp"
#include "lob/reference/reference_book.hpp"
#include "lob/sinks.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace lob {
namespace {

// Distinct types: a Qty is not a Price is not an OrderId.
static_assert(!std::is_convertible_v<Qty, Price>);
static_assert(!std::is_convertible_v<Price, Qty>);
static_assert(!std::is_convertible_v<std::int64_t, Price>);  // explicit construction only
static_assert(std::is_trivially_copyable_v<Price>);
static_assert(sizeof(Price) == 8 && sizeof(Qty) == 8 && sizeof(OrderId) == 8);

template <class T>
concept Addable = requires(T a, T b) { a + b; };
static_assert(Addable<Qty>);
static_assert(Addable<Price>);
static_assert(!Addable<OrderId>);  // arithmetic on ids is meaningless

TEST(StrongInt, ArithmeticAndComparison) {
    Qty q{10};
    q -= Qty{3};
    q += Qty{1};
    EXPECT_EQ(q, Qty{8});
    EXPECT_LT(Qty{1}, Qty{2});
    EXPECT_EQ(Qty{5} - Qty{7}, Qty{-2});
}

TEST(StrongInt, HashableAsUnorderedMapKey) {
    EXPECT_EQ(std::hash<OrderId>{}(OrderId{42}), std::hash<std::uint64_t>{}(42));
}

TEST(BookConfig, ReferenceBookRejectsInvalidConfig) {
    VectorSink sink;
    using Book = reference::ReferenceBook<VectorSink>;
    EXPECT_THROW(
        Book(BookConfig{.min_price = Price{10}, .max_price = Price{5}, .max_orders = 1}, sink),
        std::invalid_argument);
    EXPECT_THROW(
        Book(BookConfig{.min_price = Price{1}, .max_price = Price{5}, .max_orders = 0}, sink),
        std::invalid_argument);
}

TEST(Io, PrintsCommandsAndEventsReadably) {
    std::ostringstream os;
    os << limit_order(OrderId{1}, Side::Buy, Price{100}, Qty{5}) << ' '
       << Trade{.aggressor_id = OrderId{2},
                .resting_id = OrderId{1},
                .aggressor_side = Side::Sell,
                .price = Price{100},
                .qty = Qty{5}};
    EXPECT_EQ(os.str(),
              "NewOrder{id=1 Buy Limit px=100 qty=5} "
              "Trade{aggressor=2 resting=1 Sell px=100 qty=5}");
}

}  // namespace
}  // namespace lob
