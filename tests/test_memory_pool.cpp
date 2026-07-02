#include "memory_pool.h"
#include <gtest/gtest.h>
#include <memory_resource>

struct Foo {
    int x; double y;
    Foo(int a, double b) : x(a), y(b) {}
};
static_assert(sizeof(Foo) >= sizeof(void*));

TEST(FixedPool, AcquireRelease) {
    FixedPool<Foo, 4> pool;
    EXPECT_EQ(pool.free_count(), 4u);

    Foo* a = pool.construct(1, 1.0);
    Foo* b = pool.construct(2, 2.0);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->x, 1);
    EXPECT_EQ(b->x, 2);
    EXPECT_EQ(pool.used_count(), 2u);

    pool.destroy(a);
    EXPECT_EQ(pool.free_count(), 3u);
    pool.destroy(b);
    EXPECT_EQ(pool.free_count(), 4u);
}

TEST(FixedPool, ExhaustionReturnsNullptr) {
    FixedPool<Foo, 2> pool;
    pool.construct(1, 0.0);
    pool.construct(2, 0.0);
    EXPECT_TRUE(pool.full());

    Foo* extra = pool.construct(3, 0.0);
    EXPECT_EQ(extra, nullptr);
}

TEST(FixedPool, NullReleaseIsSafe) {
    FixedPool<Foo, 4> pool;
    pool.release(nullptr);  // should not crash
    EXPECT_EQ(pool.free_count(), 4u);
}

TEST(FixedPool, ReuseAfterDestroy) {
    FixedPool<Foo, 1> pool;
    Foo* a = pool.construct(10, 0.0);
    ASSERT_NE(a, nullptr);
    pool.destroy(a);

    Foo* b = pool.construct(20, 0.0);  // reuse same slot
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->x, 20);
}

TEST(PoolResource, BasicAllocate) {
    PoolResource res(4096);
    EXPECT_EQ(res.bytes_used(), 0u);

    void* p = res.allocate(64, 64);
    ASSERT_NE(p, nullptr);
    EXPECT_GE(res.bytes_used(), 64u);
}

TEST(PoolResource, AlignmentRespected) {
    PoolResource res(4096);
    void* p = res.allocate(1, 64);  // 1 byte with 64-byte alignment
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 64, 0u);
}

TEST(PoolResource, ExhaustionThrows) {
    PoolResource res(16);
    EXPECT_THROW(res.allocate(256, 1), std::bad_alloc);
}

TEST(PoolResource, ZeroCapacityThrows) {
    EXPECT_THROW(PoolResource(0), std::invalid_argument);
}

TEST(PoolResource, Reset) {
    PoolResource res(256);
    res.allocate(128, 1);
    EXPECT_GT(res.bytes_used(), 0u);
    res.reset();
    EXPECT_EQ(res.bytes_used(), 0u);
}
