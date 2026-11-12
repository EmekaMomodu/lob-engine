// Input validation and reject reasons.

#include "book_test_support.hpp"

namespace lob::test {
namespace {

template <class B>
class ValidationTest : public BookTest<B> {};
TYPED_TEST_SUITE(ValidationTest, BookImplementations);

// --- quantity ---------------------------------------------------------------

TYPED_TEST(ValidationTest, RejectsZeroQuantityLimit) {
    this->limit(1, kBuy, 100, 0);
    EXPECT_EQ(this->take(), (Events{rej(1, RejectReason::InvalidQuantity)}));
    EXPECT_EQ(this->book.order_count(), 0U);
}

TYPED_TEST(ValidationTest, RejectsNegativeQuantityLimit) {
    this->limit(1, kSell, 100, -5);
    EXPECT_EQ(this->take(), (Events{rej(1, RejectReason::InvalidQuantity)}));
    EXPECT_EQ(this->book.order_count(), 0U);
}

TYPED_TEST(ValidationTest, RejectsZeroAndNegativeQuantityMarket) {
    this->market(1, kBuy, 0);
    this->market(2, kSell, -1);
    EXPECT_EQ(this->take(), (Events{rej(1, RejectReason::InvalidQuantity),
                                    rej(2, RejectReason::InvalidQuantity)}));
}

TYPED_TEST(ValidationTest, AcceptsMaxQuantityRejectsAboveIt) {
    this->limit(1, kBuy, 100, kMaxOrderQty.value());
    this->limit(2, kBuy, 100, kMaxOrderQty.value() + 1);
    EXPECT_EQ(this->take(), (Events{acc(1), rej(2, RejectReason::InvalidQuantity)}));
}

// --- price band -------------------------------------------------------------

TYPED_TEST(ValidationTest, RejectsLimitPriceOutsideBand) {
    const auto min = this->kConfig.min_price.value();
    const auto max = this->kConfig.max_price.value();
    this->limit(1, kBuy, min - 1, 10);
    this->limit(2, kSell, max + 1, 10);
    this->limit(3, kBuy, 0, 10);
    this->limit(4, kBuy, -100, 10);
    EXPECT_EQ(this->take(),
              (Events{rej(1, RejectReason::PriceOutOfBand), rej(2, RejectReason::PriceOutOfBand),
                      rej(3, RejectReason::PriceOutOfBand), rej(4, RejectReason::PriceOutOfBand)}));
    EXPECT_EQ(this->book.order_count(), 0U);
}

TYPED_TEST(ValidationTest, AcceptsLimitPricesAtBandEdges) {
    this->limit(1, kBuy, this->kConfig.min_price.value(), 10);
    this->limit(2, kSell, this->kConfig.max_price.value(), 10);
    EXPECT_EQ(this->take(), (Events{acc(1), acc(2)}));
}

TYPED_TEST(ValidationTest, MarketOrderPriceIsIgnored) {
    this->limit(1, kSell, 100, 5);
    this->take();
    // market_order() sets price 0, which is outside the band; that must not matter.
    this->market(2, kBuy, 5);
    EXPECT_EQ(this->take(), (Events{acc(2), trade(2, 1, kBuy, 100, 5), filled(1), filled(2)}));
}

// --- order ids ----------------------------------------------------------------

TYPED_TEST(ValidationTest, RejectsDuplicateIdOfRestingOrder) {
    this->limit(1, kBuy, 100, 10);
    this->take();
    this->limit(1, kSell, 105, 3);  // different side/price: still a duplicate
    this->market(1, kSell, 3);
    EXPECT_EQ(this->take(), (Events{rej(1, RejectReason::DuplicateOrderId),
                                    rej(1, RejectReason::DuplicateOrderId)}));
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{1}));
    EXPECT_EQ(this->qty_at(kBuy, 100), 10);
}

TYPED_TEST(ValidationTest, PartiallyFilledOrderIdIsStillLive) {
    this->limit(1, kBuy, 100, 10);
    this->market(2, kSell, 4);
    this->take();
    this->limit(1, kBuy, 99, 1);
    EXPECT_EQ(this->take(), (Events{rej(1, RejectReason::DuplicateOrderId)}));
}

TYPED_TEST(ValidationTest, IdCanBeReusedAfterFill) {
    this->limit(1, kBuy, 100, 5);
    this->market(2, kSell, 5);
    this->take();
    this->limit(1, kBuy, 100, 5);
    EXPECT_EQ(this->take(), (Events{acc(1)}));
}

TYPED_TEST(ValidationTest, IdCanBeReusedAfterCancel) {
    this->limit(1, kBuy, 100, 5);
    this->cancel(1);
    this->take();
    this->limit(1, kSell, 200, 5);
    EXPECT_EQ(this->take(), (Events{acc(1)}));
}

TYPED_TEST(ValidationTest, FullyFilledAggressorIdIsNotLive) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kBuy, 100, 5);  // fully fills, never rests
    this->take();
    this->limit(2, kBuy, 90, 1);
    EXPECT_EQ(this->take(), (Events{acc(2)}));
}

// --- capacity -----------------------------------------------------------------

TYPED_TEST(ValidationTest, RejectsLimitWhenCapacityExhausted) {
    const auto capacity = static_cast<std::uint64_t>(this->kConfig.max_orders);
    for (std::uint64_t id = 1; id <= capacity; ++id) {
        this->limit(id, kBuy, 100, 1);
    }
    this->take();
    ASSERT_EQ(this->book.order_count(), capacity);

    // Rejected even though it would fully fill: capacity is checked up front.
    this->limit(100, kSell, 100, 1);
    EXPECT_EQ(this->take(), (Events{rej(100, RejectReason::CapacityExceeded)}));

    // Market orders never rest, so they are not subject to capacity.
    this->market(101, kSell, 1);
    EXPECT_EQ(this->take(),
              (Events{acc(101), trade(101, 1, kSell, 100, 1), filled(1), filled(101)}));

    // A slot was freed by the fill.
    this->limit(102, kBuy, 100, 1);
    EXPECT_EQ(this->take(), (Events{acc(102)}));
}

// --- validation order -----------------------------------------------------------

TYPED_TEST(ValidationTest, QuantityIsCheckedBeforePriceAndId) {
    this->limit(1, kBuy, 100, 10);
    this->take();
    this->limit(1, kBuy, 0, 0);  // bad qty, bad price, duplicate id
    this->limit(2, kBuy, 0, 5);  // bad price only
    EXPECT_EQ(this->take(), (Events{rej(1, RejectReason::InvalidQuantity),
                                    rej(2, RejectReason::PriceOutOfBand)}));
}

TYPED_TEST(ValidationTest, PriceIsCheckedBeforeDuplicateId) {
    this->limit(1, kBuy, 100, 10);
    this->take();
    this->limit(1, kBuy, 0, 5);
    EXPECT_EQ(this->take(), (Events{rej(1, RejectReason::PriceOutOfBand)}));
}

// --- cancel -------------------------------------------------------------------

TYPED_TEST(ValidationTest, RejectsCancelOfUnknownId) {
    this->cancel(42);
    EXPECT_EQ(this->take(), (Events{rej(42, RejectReason::UnknownOrderId)}));
}

TYPED_TEST(ValidationTest, RejectsSecondCancel) {
    this->limit(1, kBuy, 100, 10);
    this->cancel(1);
    this->cancel(1);
    EXPECT_EQ(this->take(),
              (Events{acc(1), cancelled(1, 10), rej(1, RejectReason::UnknownOrderId)}));
}

TYPED_TEST(ValidationTest, RejectsCancelOfFilledOrder) {
    this->limit(1, kBuy, 100, 5);
    this->market(2, kSell, 5);
    this->take();
    this->cancel(1);
    this->cancel(2);  // market orders never rest
    EXPECT_EQ(this->take(),
              (Events{rej(1, RejectReason::UnknownOrderId), rej(2, RejectReason::UnknownOrderId)}));
}

// --- modify -------------------------------------------------------------------

TYPED_TEST(ValidationTest, RejectsModifyOfUnknownId) {
    this->modify(7, 100, 10);
    EXPECT_EQ(this->take(), (Events{rej(7, RejectReason::UnknownOrderId)}));
}

TYPED_TEST(ValidationTest, RejectsModifyWithNegativeOrExcessiveQuantity) {
    this->limit(1, kBuy, 100, 10);
    this->take();
    this->modify(1, 100, -1);
    this->modify(1, 100, kMaxOrderQty.value() + 1);
    EXPECT_EQ(this->take(), (Events{rej(1, RejectReason::InvalidQuantity),
                                    rej(1, RejectReason::InvalidQuantity)}));
    EXPECT_EQ(this->qty_at(kBuy, 100), 10);
}

TYPED_TEST(ValidationTest, RejectsModifyToPriceOutsideBandAndLeavesOrderUntouched) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kBuy, 100, 10);
    this->take();
    this->modify(1, this->kConfig.max_price.value() + 1, 10);
    this->modify(1, this->kConfig.min_price.value() - 1, 0);  // price checked before qty<=filled
    EXPECT_EQ(this->take(),
              (Events{rej(1, RejectReason::PriceOutOfBand), rej(1, RejectReason::PriceOutOfBand)}));
    EXPECT_EQ(this->queue_at(kBuy, 100), (std::vector<std::uint64_t>{1, 2}));
}

}  // namespace
}  // namespace lob::test
