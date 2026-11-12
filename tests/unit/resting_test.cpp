// Non-crossing orders: resting, best prices, level ordering, FIFO queues.

#include "book_test_support.hpp"

namespace lob::test {
namespace {

template <class B>
class RestingTest : public BookTest<B> {};
TYPED_TEST_SUITE(RestingTest, BookImplementations);

TYPED_TEST(RestingTest, EmptyBookHasNoBestPrices) {
    EXPECT_EQ(this->best_bid(), std::nullopt);
    EXPECT_EQ(this->best_ask(), std::nullopt);
    EXPECT_EQ(this->book.order_count(), 0U);
    EXPECT_EQ(this->book.snapshot(), BookSnapshot{});
}

TYPED_TEST(RestingTest, NonCrossingLimitsRestAndEmitOnlyAccepted) {
    this->limit(1, kBuy, 99, 10);
    this->limit(2, kSell, 101, 7);
    EXPECT_EQ(this->take(), (Events{acc(1), acc(2)}));
    EXPECT_EQ(this->best_bid(), 99);
    EXPECT_EQ(this->best_ask(), 101);
    EXPECT_EQ(this->book.order_count(), 2U);
}

TYPED_TEST(RestingTest, AdjacentTicksDoNotCross) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kSell, 101, 10);
    this->limit(3, kSell, 101, 10);
    this->limit(4, kBuy, 100, 10);
    EXPECT_EQ(this->take(), (Events{acc(1), acc(2), acc(3), acc(4)}));
}

TYPED_TEST(RestingTest, BestPricesTrackTopOfEachSide) {
    this->limit(1, kBuy, 98, 1);
    this->limit(2, kBuy, 99, 1);
    this->limit(3, kBuy, 97, 1);
    this->limit(4, kSell, 103, 1);
    this->limit(5, kSell, 101, 1);
    this->limit(6, kSell, 102, 1);
    EXPECT_EQ(this->best_bid(), 99);
    EXPECT_EQ(this->best_ask(), 101);
}

TYPED_TEST(RestingTest, SnapshotListsLevelsBestFirstWithTotals) {
    this->limit(1, kBuy, 98, 5);
    this->limit(2, kBuy, 99, 3);
    this->limit(3, kBuy, 98, 2);
    this->limit(4, kSell, 102, 4);
    this->limit(5, kSell, 101, 6);

    const BookSnapshot expected{
        .bids = {LevelView{.price = Price{99},
                           .total = Qty{3},
                           .orders = {{.id = OrderId{2}, .leaves = Qty{3}}}},
                 LevelView{.price = Price{98},
                           .total = Qty{7},
                           .orders = {{.id = OrderId{1}, .leaves = Qty{5}},
                                      {.id = OrderId{3}, .leaves = Qty{2}}}}},
        .asks = {LevelView{.price = Price{101},
                           .total = Qty{6},
                           .orders = {{.id = OrderId{5}, .leaves = Qty{6}}}},
                 LevelView{.price = Price{102},
                           .total = Qty{4},
                           .orders = {{.id = OrderId{4}, .leaves = Qty{4}}}}},
    };
    EXPECT_EQ(this->book.snapshot(), expected);
}

TYPED_TEST(RestingTest, OrdersAtSameLevelQueueInArrivalOrder) {
    this->limit(3, kSell, 100, 1);
    this->limit(1, kSell, 100, 2);
    this->limit(2, kSell, 100, 3);
    EXPECT_EQ(this->queue_at(kSell, 100), (std::vector<std::uint64_t>{3, 1, 2}));
    EXPECT_EQ(this->qty_at(kSell, 100), 6);
}

}  // namespace
}  // namespace lob::test
