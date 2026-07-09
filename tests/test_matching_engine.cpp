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
    (void)me.cancel(o->id);  // resting maker — clean up
}

TEST_F(METest, ExactMatchFillsBoth) {
    (void)me.submit(Side::ASK, OrderType::LIMIT, 10000, 100);
    auto* bid = me.submit(Side::BID, OrderType::LIMIT, 10000, 100);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 100u);
    EXPECT_EQ(trades[0].price, 10000);
    EXPECT_EQ(bid->status, OrderStatus::FILLED);
    EXPECT_TRUE(me.book().empty());
    me.release(bid);  // taker filled — caller owns, must release
}

TEST_F(METest, PartialFill) {
    (void)me.submit(Side::ASK, OrderType::LIMIT, 10000, 60);
    auto* bid = me.submit(Side::BID, OrderType::LIMIT, 10000, 100);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 60u);

    // Bid partially filled — 40 remains on book
    ASSERT_NE(me.book().best_bid(), nullptr);
    EXPECT_EQ(me.book().best_bid()->total_qty, 40u);
    EXPECT_EQ(bid->filled_qty, 60u);
    EXPECT_EQ(bid->status, OrderStatus::PARTIAL);
    (void)me.cancel(bid->id);  // partial bid rests on book — clean up
}

TEST_F(METest, PricePriority) {
    // Two asks at different prices -- lower price matches first
    auto* ask1 = me.submit(Side::ASK, OrderType::LIMIT, 10010, 50);  // stays on book
    (void)me.submit(Side::ASK, OrderType::LIMIT, 10000, 50);  // auto-filled by bid
    auto* bid = me.submit(Side::BID, OrderType::LIMIT, 10020, 50);  // crosses best ask

    ASSERT_GE(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 10000);  // best ask filled first
    // bid filled exactly 50 (only best ask at 10000 matches qty=50) -- still on book or filled
    if (bid && bid->status == OrderStatus::FILLED) me.release(bid);
    else if (bid) (void)me.cancel(bid->id);
    (void)me.cancel(ask1->id);  // ask1 at 10010 was never filled — clean up
}

TEST_F(METest, TimePriority) {
    // Two asks at same price — earlier one fills first (FIFO)
    auto* ask1 = me.submit(Side::ASK, OrderType::LIMIT, 10000, 30);
    auto* ask2 = me.submit(Side::ASK, OrderType::LIMIT, 10000, 30);

    // CRITICAL: save IDs before the bid triggers a fill — the engine auto-releases
    // the filled maker (ask1) via m_pool.destroy(), which overwrites ask1->id with
    // the free-list next pointer.  Dereferencing ask1 after fill is UB.
    const OrderId ask1_id = ask1->id;
    const OrderId ask2_id = ask2->id;

    auto* bid = me.submit(Side::BID, OrderType::LIMIT, 10000, 30);  // fills ask1

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, ask1_id);  // FIFO: ask1 before ask2
    // ask1 was auto-released by the engine — cannot dereference ask1 here
    EXPECT_EQ(ask2->status, OrderStatus::NEW);  // ask2 still resting

    (void)me.cancel(ask2_id);   // clean up ask2 from book
    if (bid) me.release(bid);  // bid (taker) was filled — caller must release
}

TEST_F(METest, MarketOrderFills) {
    (void)me.submit(Side::ASK, OrderType::LIMIT, 10000, 100);
    auto* mkt = me.submit(Side::BID, OrderType::MARKET, 0, 100);

    ASSERT_NE(mkt, nullptr);
    EXPECT_EQ(mkt->status, OrderStatus::FILLED);
    EXPECT_EQ(trades.size(), 1u);
    me.release(mkt);  // filled market order — caller must release
}

TEST_F(METest, MarketOrderRejectedIfEmpty) {
    // No asks -> market buy should be rejected
    auto* mkt = me.submit(Side::BID, OrderType::MARKET, 0, 100);
    ASSERT_NE(mkt, nullptr);
    EXPECT_EQ(mkt->status, OrderStatus::REJECTED);
    EXPECT_TRUE(trades.empty());
    me.release(mkt);  // rejected — caller must release
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
        (void)me.submit(Side::ASK, OrderType::LIMIT, 10000, 10);

    // Single bid sweeps all three — save pointer to release (taker, caller owns)
    auto* bid = me.submit(Side::BID, OrderType::LIMIT, 10000, 30);

    EXPECT_EQ(trades.size(), 3u);
    EXPECT_TRUE(me.book().empty());
    if (bid) me.release(bid);  // bid is FILLED — return slot to pool
}

TEST_F(METest, CallbackExceptionIsCaught) {
    me.on_trade([](const Trade&) { throw std::runtime_error("cb error"); });
    (void)me.submit(Side::ASK, OrderType::LIMIT, 10000, 50);
    auto* bid = me.submit(Side::BID, OrderType::LIMIT, 10000, 50);
    // Engine must NOT propagate the callback exception
    EXPECT_FALSE(me.last_cb_error().empty());
    EXPECT_NE(me.last_cb_error().find("cb error"), std::string::npos);
    me.release(bid);
}

TEST_F(METest, ReleaseNullptrIsSafe) {
    me.release(nullptr);  // must not crash
}
