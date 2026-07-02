#include "order_types.h"
#include <gtest/gtest.h>

TEST(OrderTypes, CacheLine) {
    EXPECT_EQ(sizeof(Order), 64u);
    EXPECT_EQ(alignof(Order), 64u);
}

TEST(OrderTypes, DefaultValues) {
    Order o;
    EXPECT_EQ(o.id,         kInvalidOrderId);
    EXPECT_EQ(o.qty,        0u);
    EXPECT_EQ(o.filled_qty, 0u);
    EXPECT_EQ(o.status,     OrderStatus::NEW);
}

TEST(OrderTypes, Remaining) {
    Order o;
    o.qty        = 100;
    o.filled_qty = 40;
    EXPECT_EQ(o.remaining(), 60u);
    EXPECT_TRUE(o.is_active());

    o.status = OrderStatus::FILLED;
    EXPECT_FALSE(o.is_active());
}

TEST(OrderTypes, TradeValid) {
    Trade t;
    t.maker_id = 1; t.taker_id = 2; t.qty = 10;
    EXPECT_TRUE(t.valid());

    t.maker_id = kInvalidOrderId;
    EXPECT_FALSE(t.valid());
}

TEST(OrderTypes, PriceLevelDefault) {
    PriceLevel pl;
    EXPECT_EQ(pl.qty, 0u);
    EXPECT_EQ(pl.count, 0);
}
