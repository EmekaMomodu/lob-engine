// Crossing limit orders: price-time priority, partial fills, sweeps.

#include "book_test_support.hpp"

namespace lob::test {
namespace {

template <class B>
class LimitMatchingTest : public BookTest<B> {};
TYPED_TEST_SUITE(LimitMatchingTest, BookImplementations);

TYPED_TEST(LimitMatchingTest, ExactCrossFillsBothOrders) {
    this->limit(1, kSell, 100, 10);
    this->limit(2, kBuy, 100, 10);
    EXPECT_EQ(this->take(),
              (Events{acc(1), acc(2), trade(2, 1, kBuy, 100, 10), filled(1), filled(2)}));
    EXPECT_EQ(this->book.order_count(), 0U);
    EXPECT_EQ(this->best_bid(), std::nullopt);
    EXPECT_EQ(this->best_ask(), std::nullopt);
}

TYPED_TEST(LimitMatchingTest, TradesAtRestingPriceNotAggressorPrice) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kBuy, 105, 5);
    this->limit(3, kBuy, 90, 5);
    this->limit(4, kSell, 80, 5);
    EXPECT_EQ(this->take(),
              (Events{acc(1), acc(2), trade(2, 1, kBuy, 100, 5), filled(1), filled(2), acc(3),
                      acc(4), trade(4, 3, kSell, 90, 5), filled(3), filled(4)}));
}

TYPED_TEST(LimitMatchingTest, RestingOrderPartiallyFilled) {
    this->limit(1, kSell, 100, 10);
    this->take();
    this->limit(2, kBuy, 100, 4);
    EXPECT_EQ(this->take(), (Events{acc(2), trade(2, 1, kBuy, 100, 4), partial(1, 6), filled(2)}));
    EXPECT_EQ(this->queue_at(kSell, 100), (std::vector<std::uint64_t>{1}));
    EXPECT_EQ(this->qty_at(kSell, 100), 6);
}

TYPED_TEST(LimitMatchingTest, AggressorRemainderRestsAtItsLimit) {
    this->limit(1, kSell, 100, 5);
    this->take();
    this->limit(2, kBuy, 101, 8);
    EXPECT_EQ(this->take(), (Events{acc(2), trade(2, 1, kBuy, 100, 5), filled(1), partial(2, 3)}));
    EXPECT_EQ(this->best_bid(), 101);
    EXPECT_EQ(this->qty_at(kBuy, 101), 3);
    EXPECT_EQ(this->best_ask(), std::nullopt);
}

TYPED_TEST(LimitMatchingTest, SweepsMultipleLevelsInPriceOrder) {
    this->limit(1, kSell, 102, 5);
    this->limit(2, kSell, 100, 5);
    this->limit(3, kSell, 101, 5);
    this->limit(4, kSell, 103, 5);
    this->take();

    this->limit(9, kBuy, 102, 12);
    EXPECT_EQ(this->take(), (Events{acc(9), trade(9, 2, kBuy, 100, 5), filled(2), partial(9, 7),
                                    trade(9, 3, kBuy, 101, 5), filled(3), partial(9, 2),
                                    trade(9, 1, kBuy, 102, 2), partial(1, 3), filled(9)}));
    EXPECT_EQ(this->best_ask(), 102);
    EXPECT_EQ(this->qty_at(kSell, 102), 3);
    EXPECT_EQ(this->qty_at(kSell, 103), 5);
    EXPECT_EQ(this->best_bid(), std::nullopt);
}

TYPED_TEST(LimitMatchingTest, DoesNotTradeThroughItsLimit) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kSell, 102, 5);
    this->take();
    this->limit(9, kBuy, 101, 8);
    EXPECT_EQ(this->take(), (Events{acc(9), trade(9, 1, kBuy, 100, 5), filled(1), partial(9, 3)}));
    EXPECT_EQ(this->best_bid(), 101);
    EXPECT_EQ(this->qty_at(kBuy, 101), 3);
    EXPECT_EQ(this->qty_at(kSell, 102), 5);
}

TYPED_TEST(LimitMatchingTest, ConsumesLevelInTimePriority) {
    this->limit(1, kSell, 100, 3);
    this->limit(2, kSell, 100, 3);
    this->limit(3, kSell, 100, 3);
    this->take();
    this->limit(9, kBuy, 100, 7);
    EXPECT_EQ(this->take(), (Events{acc(9), trade(9, 1, kBuy, 100, 3), filled(1), partial(9, 4),
                                    trade(9, 2, kBuy, 100, 3), filled(2), partial(9, 1),
                                    trade(9, 3, kBuy, 100, 1), partial(3, 2), filled(9)}));
    EXPECT_EQ(this->queue_at(kSell, 100), (std::vector<std::uint64_t>{3}));
}

TYPED_TEST(LimitMatchingTest, SellAggressorSweepsBidsHighestFirst) {
    this->limit(1, kBuy, 98, 4);
    this->limit(2, kBuy, 100, 4);
    this->limit(3, kBuy, 99, 4);
    this->take();
    this->limit(9, kSell, 99, 10);
    EXPECT_EQ(this->take(), (Events{acc(9), trade(9, 2, kSell, 100, 4), filled(2), partial(9, 6),
                                    trade(9, 3, kSell, 99, 4), filled(3), partial(9, 2)}));
    EXPECT_EQ(this->best_ask(), 99);
    EXPECT_EQ(this->qty_at(kSell, 99), 2);
    EXPECT_EQ(this->best_bid(), 98);
}

TYPED_TEST(LimitMatchingTest, LaterArrivalAtBetterPriceHasPriority) {
    this->limit(1, kBuy, 100, 5);
    this->limit(2, kBuy, 101, 5);  // arrives later but better price
    this->take();
    this->limit(9, kSell, 100, 5);
    EXPECT_EQ(this->take(), (Events{acc(9), trade(9, 2, kSell, 101, 5), filled(2), filled(9)}));
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{1}));
}

TYPED_TEST(LimitMatchingTest, OrderNeverMatchesSameSide) {
    this->limit(1, kBuy, 100, 5);
    this->limit(2, kBuy, 105, 5);
    EXPECT_EQ(this->take(), (Events{acc(1), acc(2)}));
    EXPECT_EQ(this->book.order_count(), 2U);
}

}  // namespace
}  // namespace lob::test
