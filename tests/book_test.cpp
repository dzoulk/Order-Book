// Milestone 2: the same behavioral test suite runs against the naive
// reference book, the real v1 OrderBook, and the v2 FastOrderBook via a
// GoogleTest typed test suite, so all three stay in parity by construction
// rather than by duplicated test files. The milestone 3 fuzz test extends
// this same parity check to millions of random operations.

#include "orderbook/fast_order_book.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/reference_book.hpp"

#include <gtest/gtest.h>
#include <stdexcept>

using namespace orderbook;

template <typename Book>
class OrderBookTest : public ::testing::Test {
protected:
    Book book;
};

using BookTypes = ::testing::Types<NaiveOrderBook, OrderBook, FastOrderBook>;
TYPED_TEST_SUITE(OrderBookTest, BookTypes);

TYPED_TEST(OrderBookTest, RestsWhenNoCounterparty) {
    auto trades = this->book.addLimit(1, Side::Buy, 100, 10);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(this->book.bestBid(), 100);
    EXPECT_FALSE(this->book.bestAsk().has_value());
    EXPECT_EQ(this->book.depthAt(Side::Buy, 100), 10u);
}

TYPED_TEST(OrderBookTest, SimpleMatch) {
    this->book.addLimit(1, Side::Sell, 100, 10);
    auto trades = this->book.addLimit(2, Side::Buy, 100, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buyOrderId, 2u);
    EXPECT_EQ(trades[0].sellOrderId, 1u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].qty, 10u);
    EXPECT_FALSE(this->book.bestBid().has_value());
    EXPECT_FALSE(this->book.bestAsk().has_value());
}

TYPED_TEST(OrderBookTest, PartialFillLeavesRemainder) {
    this->book.addLimit(1, Side::Sell, 100, 4);
    auto trades = this->book.addLimit(2, Side::Buy, 100, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 4u);
    EXPECT_EQ(this->book.depthAt(Side::Buy, 100), 6u);
}

TYPED_TEST(OrderBookTest, FifoAtSamePrice) {
    this->book.addLimit(1, Side::Sell, 100, 5);
    this->book.addLimit(2, Side::Sell, 100, 5);
    auto trades = this->book.addLimit(3, Side::Buy, 100, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellOrderId, 1u);  // oldest resting order fills first
}

TYPED_TEST(OrderBookTest, SweepsMultipleLevels) {
    this->book.addLimit(1, Side::Sell, 100, 5);
    this->book.addLimit(2, Side::Sell, 101, 5);
    auto trades = this->book.addLimit(3, Side::Buy, 101, 10);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].qty, 5u);
    EXPECT_EQ(trades[1].price, 101);
    EXPECT_EQ(trades[1].qty, 5u);
    EXPECT_FALSE(this->book.bestAsk().has_value());
}

TYPED_TEST(OrderBookTest, CancelRemovesRestingOrder) {
    this->book.addLimit(1, Side::Buy, 100, 10);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_FALSE(this->book.bestBid().has_value());
    EXPECT_FALSE(this->book.cancel(1));  // already gone
}

TYPED_TEST(OrderBookTest, CancelAfterPartialFill) {
    this->book.addLimit(1, Side::Sell, 100, 10);
    auto trades = this->book.addLimit(2, Side::Buy, 100, 4);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(this->book.depthAt(Side::Sell, 100), 6u);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_FALSE(this->book.bestAsk().has_value());
}

TYPED_TEST(OrderBookTest, MarketOrderOnEmptyBookIsANoOp) {
    auto trades = this->book.addMarket(1, Side::Buy, 10);
    EXPECT_TRUE(trades.empty());
}

TYPED_TEST(OrderBookTest, MarketOrderSweepsAndDiscardsRemainder) {
    this->book.addLimit(1, Side::Sell, 100, 3);
    auto trades = this->book.addMarket(2, Side::Buy, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 3u);
    EXPECT_FALSE(this->book.bestAsk().has_value());
}

TYPED_TEST(OrderBookTest, DuplicateIdThrows) {
    this->book.addLimit(1, Side::Buy, 100, 10);
    EXPECT_THROW(this->book.addLimit(1, Side::Sell, 100, 5), std::invalid_argument);
}

TYPED_TEST(OrderBookTest, ZeroQtyThrows) {
    EXPECT_THROW(this->book.addLimit(1, Side::Buy, 100, 0), std::invalid_argument);
}
