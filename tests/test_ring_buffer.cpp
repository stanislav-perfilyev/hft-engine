#include "ring_buffer.h"
#include <gtest/gtest.h>
#include <thread>

TEST(RingBuffer, PushPopBasic) {
    RingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.empty());
    EXPECT_TRUE(rb.push(1));
    EXPECT_TRUE(rb.push(2));
    int v = 0;
    EXPECT_TRUE(rb.pop(v)); EXPECT_EQ(v, 1);
    EXPECT_TRUE(rb.pop(v)); EXPECT_EQ(v, 2);
    EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, FillAndDrain) {
    RingBuffer<int, 4> rb;
    // Capacity=4 → wastes 1 slot → max 3 items
    EXPECT_TRUE(rb.push(10));
    EXPECT_TRUE(rb.push(20));
    EXPECT_TRUE(rb.push(30));
    EXPECT_TRUE(rb.full());
    EXPECT_FALSE(rb.push(40));  // should fail when full

    auto v1 = rb.pop(); ASSERT_TRUE(v1.has_value()); EXPECT_EQ(*v1, 10);
    auto v2 = rb.pop(); ASSERT_TRUE(v2.has_value()); EXPECT_EQ(*v2, 20);
    auto v3 = rb.pop(); ASSERT_TRUE(v3.has_value()); EXPECT_EQ(*v3, 30);
    EXPECT_FALSE(rb.pop().has_value());
}

TEST(RingBuffer, WrapAround) {
    RingBuffer<int, 4> rb;
    for (int round = 0; round < 10; ++round) {
        EXPECT_TRUE(rb.push(round));
        int v = 0;
        EXPECT_TRUE(rb.pop(v));
        EXPECT_EQ(v, round);
    }
}

TEST(RingBuffer, SPSC_Concurrency) {
    static constexpr int kItems = 100000;
    RingBuffer<int, 1024> rb;
    std::vector<int> received;
    received.reserve(kItems);

    std::jthread producer([&](std::stop_token) {
        for (int i = 0; i < kItems; ++i)
            while (!rb.push(i)) std::this_thread::yield();
    });

    int got = 0;
    while (got < kItems) {
        int v = 0;
        if (rb.pop(v)) { received.push_back(v); ++got; }
        else std::this_thread::yield();
    }

    ASSERT_EQ(static_cast<int>(received.size()), kItems);
    for (int i = 0; i < kItems; ++i)
        EXPECT_EQ(received[i], i) << "mismatch at index " << i;
}
