// Market orders: IOC semantics, sweeping, empty book.

#include "book_test_support.hpp"

namespace lob::test {
namespace {

template <class B>
class MarketOrderTest : public BookTest<B> {};
TYPED_TEST_SUITE(MarketOrderTest, BookImplementations);

TYPED_TEST(MarketOrderTest, EmptyBookCancelsWholeOrderWithNoLiquidity) {
    this->market(1, kBuy, 10);
    this->market(2, kSell, 3);
    EXPECT_EQ(this->take(), (Events{acc(1), cancelled(1, 10, CancelReason::NoLiquidity), acc(2),
                                    cancelled(2, 3, CancelReason::NoLiquidity)}));
    EXPECT_EQ(this->book.order_count(), 0U);
}

TYPED_TEST(MarketOrderTest, OnlyOppositeSideProvidesLiquidity) {
    this->limit(1, kBuy, 100, 10);
    this->take();
    this->market(2, kBuy, 5);
    EXPECT_EQ(this->take(), (Events{acc(2), cancelled(2, 5, CancelReason::NoLiquidity)}));
    EXPECT_EQ(this->qty_at(kBuy, 100), 10);
}

TYPED_TEST(MarketOrderTest, FullyFilledAtSingleLevel) {
    this->limit(1, kSell, 100, 10);
    this->take();
    this->market(2, kBuy, 4);
    EXPECT_EQ(this->take(), (Events{acc(2), trade(2, 1, kBuy, 100, 4), partial(1, 6), filled(2)}));
}

TYPED_TEST(MarketOrderTest, SweepsLevelsRegardlessOfPrice) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kSell, 500, 5);
    this->limit(3, kSell, 9'000, 5);
    this->take();
    this->market(9, kBuy, 12);
    EXPECT_EQ(this->take(), (Events{acc(9), trade(9, 1, kBuy, 100, 5), filled(1), partial(9, 7),
                                    trade(9, 2, kBuy, 500, 5), filled(2), partial(9, 2),
                                    trade(9, 3, kBuy, 9'000, 2), partial(3, 3), filled(9)}));
}

TYPED_TEST(MarketOrderTest, SellSweepsBidsHighestFirst) {
    this->limit(1, kBuy, 90, 5);
    this->limit(2, kBuy, 95, 5);
    this->take();
    this->market(9, kSell, 7);
    EXPECT_EQ(this->take(), (Events{acc(9), trade(9, 2, kSell, 95, 5), filled(2), partial(9, 2),
                                    trade(9, 1, kSell, 90, 2), partial(1, 3), filled(9)}));
}

TYPED_TEST(MarketOrderTest, RemainderCancelledWhenLiquidityRunsOut) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kSell, 101, 5);
    this->take();
    this->market(9, kBuy, 15);
    EXPECT_EQ(this->take(), (Events{acc(9), trade(9, 1, kBuy, 100, 5), filled(1), partial(9, 10),
                                    trade(9, 2, kBuy, 101, 5), filled(2), partial(9, 5),
                                    cancelled(9, 5, CancelReason::NoLiquidity)}));
    EXPECT_EQ(this->book.order_count(), 0U);
    EXPECT_EQ(this->best_ask(), std::nullopt);
}

TYPED_TEST(MarketOrderTest, NeverRests) {
    this->limit(1, kSell, 100, 5);
    this->market(9, kBuy, 8);
    this->take();
    EXPECT_EQ(this->book.order_count(), 0U);
    EXPECT_EQ(this->best_bid(), std::nullopt);
    this->limit(9, kBuy, 100, 1);  // id is free again
    EXPECT_EQ(this->take(), (Events{acc(9)}));
}

}  // namespace
}  // namespace lob::test
