#include "order_book.h"
#include "memory_pool.h"
#include <gtest/gtest.h>
#include <memory>

class OrderBookTest : public ::testing::Test {
protected:
    static constexpr std::size_t kPoolBytes = 1 << 20;  // 1 MB
    PoolResource pool{kPoolBytes};
    OrderBook book{&pool};

    Order* make_order(OrderId id, Side side, Price price, Qty qty,
                      OrderType type = OrderType::LIMIT) {
        // Allocate from pool resource directly for test
        Order* o = static_cast<Order*>(pool.allocate(sizeof(Order), alignof(Order)));
        *o = Order{};
        o->id    = id;
        o->side  = side;
        o->price = price;
        o->qty   = qty;
        o->type  = type;
        o->status = OrderStatus::NEW;
        return o;
    }
};

TEST_F(OrderBookTest, EmptyBook) {
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_ask(), nullptr);
}

TEST_F(OrderBookTest, AddBid) {
    auto* o = make_order(1, Side::BID, 10000, 100);
    book.add_order(o);
    ASSERT_NE(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_bid()->price, 10000);
    EXPECT_EQ(book.best_bid()->total_qty, 100u);
    EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(OrderBookTest, BidsSortedDescending) {
    book.add_order(make_order(1, Side::BID, 9900, 50));
    book.add_order(make_order(2, Side::BID, 10100, 50));
    book.add_order(make_order(3, Side::BID, 10000, 50));

    // Best bid should be highest price
    EXPECT_EQ(book.best_bid()->price, 10100);
}

TEST_F(OrderBookTest, AsksSortedAscending) {
    book.add_order(make_order(1, Side::ASK, 10200, 50));
    book.add_order(make_order(2, Side::ASK, 10000, 50));
    book.add_order(make_order(3, Side::ASK, 10100, 50));

    // Best ask should be lowest price
    EXPECT_EQ(book.best_ask()->price, 10000);
}

TEST_F(OrderBookTest, CancelOrder) {
    auto* o = make_order(1, Side::BID, 10000, 100);
    book.add_order(o);
    EXPECT_TRUE(book.cancel_order(1));
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(OrderBookTest, CancelNonExistent) {
    EXPECT_FALSE(book.cancel_order(999));
}

TEST_F(OrderBookTest, DuplicateOrderThrows) {
    auto* o1 = make_order(1, Side::BID, 10000, 100);
    auto* o2 = make_order(1, Side::BID, 10000, 50);
    book.add_order(o1);
    EXPECT_THROW(book.add_order(o2), std::invalid_argument);
}

TEST_F(OrderBookTest, Spread) {
    book.add_order(make_order(1, Side::BID, 9990, 100));
    book.add_order(make_order(2, Side::ASK, 10010, 100));
    EXPECT_EQ(book.spread(), 20);
}

TEST_F(OrderBookTest, L2Snapshot) {
    book.add_order(make_order(1, Side::BID, 10000, 100));
    book.add_order(make_order(2, Side::BID, 9900,  50));
    book.add_order(make_order(3, Side::ASK, 10100, 75));

    auto snap = book.snapshot(2);
    ASSERT_EQ(snap.bids.size(), 2u);
    ASSERT_EQ(snap.asks.size(), 1u);
    EXPECT_EQ(snap.bids[0].price, 10000);
    EXPECT_EQ(snap.bids[1].price, 9900);
    EXPECT_EQ(snap.asks[0].price, 10100);
}
