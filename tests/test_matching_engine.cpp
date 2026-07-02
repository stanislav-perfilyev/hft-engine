#include "matching_engine.h"
#include <gtest/gtest.h>
#include <vector>

class METest : public ::testing::Test {
protected:
    MatchingEngine<1024> me;
    std::vector<Trade> trades;

    void SetUp() override {
        me.on_trade([this](const Trade& t) { trades.push_back(t); });
    }
};

TEST_F(METest, SubmitLimitRests) {
    auto* o = me.submit(Side::BID, OrderType::LIMIT, 10000, 100);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->status, OrderStatus::NEW);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(me.book().best_bid()->price, 10000);
}

TEST_F(METest, ExactMatchFillsBoth) {
    me.submit(Side::ASK, OrderType::LIMIT, 10000, 100);
    me.submit(Side::BID, OrderType::LIMIT, 10000, 100);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 100u);
    EXPECT_EQ(trades[0].price, 10000);
    EXPECT_TRUE(me.book().empty());
}

TEST_F(METest, PartialFill) {
    me.submit(Side::ASK, OrderType::LIMIT, 10000, 60);
    auto* bid = me.submit(Side::BID, OrderType::LIMIT, 10000, 100);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 60u);

    // Bid partially filled — 40 remains on book
    ASSERT_NE(me.book().best_bid(), nullptr);
    EXPECT_EQ(me.book().best_bid()->total_qty, 40u);
    EXPECT_EQ(bid->filled_qty, 60u);
    EXPECT_EQ(bid->status, OrderStatus::PARTIAL);
}

TEST_F(METest, PricePriority) {
    // Two asks at different prices — lower price matches first
    me.submit(Side::ASK, OrderType::LIMIT, 10010, 50);
    me.submit(Side::ASK, OrderType::LIMIT, 10000, 50);
    me.submit(Side::BID, OrderType::LIMIT, 10020, 50);  // crosses both

    ASSERT_GE(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 10000);  // best ask filled first
}

TEST_F(METest, TimePriority) {
    // Two asks at same price — earlier one fills first
    auto* ask1 = me.submit(Side::ASK, OrderType::LIMIT, 10000, 30);
    auto* ask2 = me.submit(Side::ASK, OrderType::LIMIT, 10000, 30);

    me.submit(Side::BID, OrderType::LIMIT, 10000, 30);  // fills exactly one

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, ask1->id);  // FIFO: ask1 before ask2
    EXPECT_EQ(ask1->status, OrderStatus::FILLED);
    EXPECT_EQ(ask2->status, OrderStatus::NEW);
}

TEST_F(METest, MarketOrderFills) {
    me.submit(Side::ASK, OrderType::LIMIT, 10000, 100);
    auto* mkt = me.submit(Side::BID, OrderType::MARKET, 0, 100);

    ASSERT_NE(mkt, nullptr);
    EXPECT_EQ(mkt->status, OrderStatus::FILLED);
    EXPECT_EQ(trades.size(), 1u);
}

TEST_F(METest, MarketOrderRejectedIfEmpty) {
    // No asks → market buy should be rejected
    auto* mkt = me.submit(Side::BID, OrderType::MARKET, 0, 100);
    ASSERT_NE(mkt, nullptr);
    EXPECT_EQ(mkt->status, OrderStatus::REJECTED);
    EXPECT_TRUE(trades.empty());
}

TEST_F(METest, CancelOrder) {
    auto* o = me.submit(Side::BID, OrderType::LIMIT, 10000, 100);
    ASSERT_NE(o, nullptr);
    const OrderId id = o->id;

    EXPECT_TRUE(me.cancel(id));
    EXPECT_TRUE(me.book().empty());
    EXPECT_FALSE(me.cancel(id));  // already cancelled
}

TEST_F(METest, NullQtyReturnsNullptr) {
    auto* o = me.submit(Side::BID, OrderType::LIMIT, 10000, 0);
    EXPECT_EQ(o, nullptr);
}

TEST_F(METest, MultipleFillers) {
    // Resting: 3 asks of qty 10 at 10000
    for (int i = 0; i < 3; ++i)
        me.submit(Side::ASK, OrderType::LIMIT, 10000, 10);

    // Single bid sweeps all three
    me.submit(Side::BID, OrderType::LIMIT, 10000, 30);

    EXPECT_EQ(trades.size(), 3u);
    EXPECT_TRUE(me.book().empty());
}
