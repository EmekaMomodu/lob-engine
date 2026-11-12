// Cancels: removal, level cleanup, queue integrity, partially filled orders.

#include "book_test_support.hpp"

namespace lob::test {
namespace {

template <class B>
class CancelTest : public BookTest<B> {};
TYPED_TEST_SUITE(CancelTest, BookImplementations);

TYPED_TEST(CancelTest, CancelRemovesRestingOrder) {
    this->limit(1, kBuy, 100, 10);
    this->take();
    this->cancel(1);
    EXPECT_EQ(this->take(), (Events{cancelled(1, 10)}));
    EXPECT_EQ(this->book.order_count(), 0U);
    EXPECT_EQ(this->best_bid(), std::nullopt);
}

TYPED_TEST(CancelTest, CancellingLastOrderAtBestMovesBestToNextLevel) {
    this->limit(1, kSell, 100, 1);
    this->limit(2, kSell, 103, 1);
    this->limit(3, kBuy, 99, 1);
    this->limit(4, kBuy, 95, 1);
    this->cancel(1);
    this->cancel(3);
    EXPECT_EQ(this->best_ask(), 103);
    EXPECT_EQ(this->best_bid(), 95);
    EXPECT_EQ(this->book.snapshot().asks.size(), 1U);
    EXPECT_EQ(this->book.snapshot().bids.size(), 1U);
}

TYPED_TEST(CancelTest, CancelFromMiddleOfQueuePreservesOrderOfOthers) {
    this->limit(1, kBuy, 100, 2);
    this->limit(2, kBuy, 100, 2);
    this->limit(3, kBuy, 100, 2);
    this->cancel(2);
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{1, 3}));
    EXPECT_EQ(this->qty_at(kBuy, 100), 4);
    this->take();

    this->market(9, kSell, 3);
    EXPECT_EQ(this->take(), (Events{acc(9), trade(9, 1, kSell, 100, 2), filled(1), partial(9, 1),
                                    trade(9, 3, kSell, 100, 1), partial(3, 1), filled(9)}));
}

TYPED_TEST(CancelTest, CancelHeadAndTailOfQueue) {
    this->limit(1, kSell, 100, 1);
    this->limit(2, kSell, 100, 1);
    this->limit(3, kSell, 100, 1);
    this->cancel(1);
    this->cancel(3);
    EXPECT_EQ(this->queue_at(kSell, 100), (std::vector<std::uint64_t>{2}));
    this->limit(4, kSell, 100, 1);  // appends behind 2
    EXPECT_EQ(this->queue_at(kSell, 100), (std::vector<std::uint64_t>{2, 4}));
}

TYPED_TEST(CancelTest, CancelPartiallyFilledOrderReportsRemainingQuantity) {
    this->limit(1, kSell, 100, 10);
    this->market(2, kBuy, 4);
    this->take();
    this->cancel(1);
    EXPECT_EQ(this->take(), (Events{cancelled(1, 6)}));
    EXPECT_EQ(this->book.order_count(), 0U);
}

TYPED_TEST(CancelTest, CancelDoesNotAffectOtherSide) {
    this->limit(1, kBuy, 100, 5);
    this->limit(2, kSell, 101, 5);
    this->cancel(1);
    EXPECT_EQ(this->best_ask(), 101);
    EXPECT_EQ(this->qty_at(kSell, 101), 5);
}

}  // namespace
}  // namespace lob::test
