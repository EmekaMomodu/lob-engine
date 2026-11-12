// Modify (cancel/replace): priority rules, total-quantity semantics, crossing.
//
// Modify.qty is the new TOTAL order quantity including what has already
// filled (FIX OrderQty semantics); leaves = qty - filled.

#include "book_test_support.hpp"

namespace lob::test {
namespace {

template <class B>
class ModifyTest : public BookTest<B> {};
TYPED_TEST_SUITE(ModifyTest, BookImplementations);

// --- priority rules -------------------------------------------------------------

TYPED_TEST(ModifyTest, QuantityDecreaseKeepsPriority) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kBuy, 100, 10);
    this->take();
    this->modify(1, 100, 4);
    EXPECT_EQ(this->take(), (Events{modified(1, 100, 4, true)}));
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(this->qty_at(kBuy, 100), 14);

    this->market(9, kSell, 5);
    EXPECT_EQ(this->take(), (Events{acc(9), trade(9, 1, kSell, 100, 4), filled(1), partial(9, 1),
                                    trade(9, 2, kSell, 100, 1), partial(2, 9), filled(9)}));
}

TYPED_TEST(ModifyTest, QuantityIncreaseLosesPriority) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kBuy, 100, 10);
    this->take();
    this->modify(1, 100, 15);
    EXPECT_EQ(this->take(), (Events{modified(1, 100, 15, false)}));
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{2, 1}));
    EXPECT_EQ(this->qty_at(kBuy, 100), 25);
}

TYPED_TEST(ModifyTest, PriceChangeLosesPriorityAndMovesLevel) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kBuy, 100, 10);
    this->take();
    this->modify(1, 99, 10);
    EXPECT_EQ(this->take(), (Events{modified(1, 99, 10, false)}));
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{2}));
    EXPECT_EQ(this->queue_at(kBuy, 99), (std::vector<std::uint64_t>{1}));
    EXPECT_EQ(this->best_bid(), 100);
}

TYPED_TEST(ModifyTest, MovingBackToOriginalPriceJoinsBackOfQueue) {
    this->limit(1, kSell, 100, 10);
    this->limit(2, kSell, 100, 10);
    this->modify(1, 101, 10);
    this->modify(1, 100, 10);
    EXPECT_EQ(this->queue_at(kSell, 100), (std::vector<std::uint64_t>{2, 1}));
    EXPECT_EQ(this->queue_at(kSell, 101), (std::vector<std::uint64_t>{}));
}

TYPED_TEST(ModifyTest, PriceChangeWithQuantityDecreaseStillLosesPriority) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kBuy, 101, 10);
    this->take();
    this->modify(1, 101, 5);
    EXPECT_EQ(this->take(), (Events{modified(1, 101, 5, false)}));
    EXPECT_EQ(this->queue_at(kBuy, 101), (std::vector<std::uint64_t>{2, 1}));
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{}));
}

TYPED_TEST(ModifyTest, UnchangedModifyIsAcceptedAndKeepsPriority) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kBuy, 100, 10);
    this->take();
    this->modify(1, 100, 10);
    EXPECT_EQ(this->take(), (Events{modified(1, 100, 10, true)}));
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{1, 2}));
}

// --- crossing on modify -----------------------------------------------------------

TYPED_TEST(ModifyTest, ModifyToCrossingPriceMatchesImmediatelyAndRestsRemainder) {
    this->limit(5, kSell, 101, 4);
    this->limit(1, kBuy, 100, 10);
    this->take();
    this->modify(1, 101, 10);
    EXPECT_EQ(this->take(), (Events{modified(1, 101, 10, false), trade(1, 5, kBuy, 101, 4),
                                    filled(5), partial(1, 6)}));
    EXPECT_EQ(this->best_bid(), 101);
    EXPECT_EQ(this->qty_at(kBuy, 101), 6);
    EXPECT_EQ(this->best_ask(), std::nullopt);
}

TYPED_TEST(ModifyTest, ModifyToCrossingPriceCanFullyFill) {
    this->limit(5, kBuy, 100, 10);
    this->limit(1, kSell, 105, 3);
    this->take();
    this->modify(1, 99, 3);  // sweeps the 100 bid at the resting price
    EXPECT_EQ(this->take(), (Events{modified(1, 99, 3, false), trade(1, 5, kSell, 100, 3),
                                    partial(5, 7), filled(1)}));
    EXPECT_EQ(this->book.order_count(), 1U);
    EXPECT_EQ(this->best_ask(), std::nullopt);
}

// --- total-quantity semantics on partially filled orders --------------------------

TYPED_TEST(ModifyTest, DecreaseOnPartiallyFilledOrderUsesTotalQuantity) {
    this->limit(1, kBuy, 100, 10);
    this->market(9, kSell, 4);  // filled 4, leaves 6
    this->take();
    this->modify(1, 100, 8);  // total 8 -> leaves 8 - 4 = 4
    EXPECT_EQ(this->take(), (Events{modified(1, 100, 4, true)}));
    EXPECT_EQ(this->qty_at(kBuy, 100), 4);
}

TYPED_TEST(ModifyTest, IncreaseOnPartiallyFilledOrderUsesTotalQuantity) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kBuy, 100, 10);
    this->market(9, kSell, 4);  // order 1: filled 4, leaves 6
    this->take();
    this->modify(1, 100, 12);  // total 12 -> leaves 8, back of queue
    EXPECT_EQ(this->take(), (Events{modified(1, 100, 8, false)}));
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{2, 1}));
    EXPECT_EQ(this->qty_at(kBuy, 100), 18);
}

TYPED_TEST(ModifyTest, ModifyToFilledQuantityCancels) {
    this->limit(1, kBuy, 100, 10);
    this->market(9, kSell, 4);
    this->take();
    this->modify(1, 100, 4);
    EXPECT_EQ(this->take(), (Events{cancelled(1, 6, CancelReason::ReducedToFilled)}));
    EXPECT_EQ(this->book.order_count(), 0U);
}

TYPED_TEST(ModifyTest, ModifyBelowFilledQuantityCancels) {
    this->limit(1, kSell, 100, 10);
    this->market(9, kBuy, 4);
    this->take();
    this->modify(1, 100, 1);
    EXPECT_EQ(this->take(), (Events{cancelled(1, 6, CancelReason::ReducedToFilled)}));
    EXPECT_EQ(this->best_ask(), std::nullopt);
}

TYPED_TEST(ModifyTest, ModifyToZeroCancels) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kBuy, 100, 10);
    this->take();
    this->modify(1, 100, 0);
    EXPECT_EQ(this->take(), (Events{cancelled(1, 10, CancelReason::ReducedToFilled)}));
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{2}));
}

TYPED_TEST(ModifyTest, ModifiedOrderCanBeCancelledAndIdReused) {
    this->limit(1, kBuy, 100, 10);
    this->modify(1, 101, 12);
    this->cancel(1);
    this->limit(1, kSell, 200, 1);
    EXPECT_EQ(this->take(),
              (Events{acc(1), modified(1, 101, 12, false), cancelled(1, 12), acc(1)}));
}

}  // namespace
}  // namespace lob::test
