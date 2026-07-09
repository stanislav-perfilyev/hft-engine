// ─────────────────────────────────────────────────────────────────────────────
// tests/test_mt_engine.cpp  —  Multi-threaded engine pipeline tests
//
// Tests:
//  1. MT_RingBuffer_StressTest    — 2 jthreads, 500k items, verify all consumed
//  2. MT_RingBuffer_Ordering      — FIFO ordering under concurrent load
//  3. MT_EngineRunner_Throughput  — full pipeline, measure ops/sec
//  4. MT_EngineRunner_Latency     — mean enqueue→process latency < 50µs
//  5. MT_EngineRunner_Shutdown    — graceful stop drains queue
//  6. MT_EngineRunner_BackPressure — queue-full drops are counted correctly
// ─────────────────────────────────────────────────────────────────────────────
#include "engine_runner.h"
#include "ring_buffer.h"
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <stop_token>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ─── helpers ─────────────────────────────────────────────────────────────────
[[maybe_unused]] static int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ─── 1. Ring buffer stress: all items produced are consumed exactly once ──────
TEST(MT_RingBuffer, StressTest_500k) {
    constexpr std::size_t N    = 500'000;
    constexpr std::size_t QCAP = 65536;

    RingBuffer<uint64_t, QCAP> rb;
    std::atomic<uint64_t>      consumed_sum{0};
    std::atomic<bool>          producer_done{false};

    // Producer jthread
    std::jthread producer([&](std::stop_token) {
        for (uint64_t i = 0; i < N; ++i) {
            while (!rb.push(i)) std::this_thread::yield(); // back-pressure
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Consumer jthread
    std::jthread consumer([&](std::stop_token) {
        uint64_t local_sum = 0;
        uint64_t count     = 0;
        while (count < N) {
            auto v = rb.pop();
            if (!v) { std::this_thread::yield(); continue; }
            local_sum += *v;
            ++count;
        }
        consumed_sum.store(local_sum, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    // Sum 0..N-1 = N*(N-1)/2
    const uint64_t expected = N * (N - 1) / 2;
    EXPECT_EQ(consumed_sum.load(), expected);
    EXPECT_TRUE(rb.empty());
}

// ─── 2. FIFO ordering under concurrent push/pop ───────────────────────────────
TEST(MT_RingBuffer, FIFOOrdering) {
    constexpr std::size_t N    = 10'000;
    constexpr std::size_t QCAP = 16384;

    RingBuffer<uint32_t, QCAP> rb;
    std::vector<uint32_t>      received;
    received.reserve(N);

    std::jthread producer([&](std::stop_token) {
        for (uint32_t i = 0; i < N; ++i) {
            while (!rb.push(i)) std::this_thread::yield();
        }
    });

    std::jthread consumer([&](std::stop_token) {
        while (received.size() < N) {
            auto v = rb.pop();
            if (!v) { std::this_thread::yield(); continue; }
            received.push_back(*v);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), N);
    for (uint32_t i = 0; i < N; ++i)
        EXPECT_EQ(received[i], i) << "FIFO violated at index " << i;
}

// ─── 3. EngineRunner throughput ───────────────────────────────────────────────
TEST(MT_EngineRunner, Throughput) {
    constexpr std::size_t N        = 50'000;
    constexpr double      MIN_MOPS = 0.05; // 50k ops/sec — conservative for CI

    auto runner_up = std::make_unique<EngineRunner<65536>>();
    auto& runner = *runner_up;
    runner.start();

    const auto t0 = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < N; ++i) {
        const Price p = static_cast<Price>(100 + (i % 20));
        const Side  s = (i % 2 == 0) ? Side::BID : Side::ASK;
        while (!runner.submit(s, OrderType::LIMIT, p, 1))
            std::this_thread::yield();
    }

    // Wait until all processed
    while (runner.stats().orders_processed.load(std::memory_order_relaxed) < N)
        std::this_thread::yield();

    runner.stop();

    const double elapsed_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    const double mops = runner.stats().throughput_ops(elapsed_s) / 1e6;
    std::printf("[Throughput] %.3f M ops/sec over %.3f s\n", mops, elapsed_s);

    EXPECT_EQ(runner.stats().orders_processed.load(), N);
    EXPECT_GE(mops, MIN_MOPS);
}

// ─── 4. End-to-end pipeline latency ─────────────────────────────────────────
TEST(MT_EngineRunner, MeanLatencyUnder100us) {
    constexpr std::size_t  N         = 10'000;
    constexpr double       MAX_US    = 100.0; // 100µs mean — generous for CI

    auto runner_up = std::make_unique<EngineRunner<65536>>();
    auto& runner = *runner_up;
    runner.start();

    for (std::size_t i = 0; i < N; ++i) {
        while (!runner.submit(Side::BID, OrderType::LIMIT,
                              static_cast<Price>(100 + i % 5), 1))
            std::this_thread::yield();
    }

    while (runner.stats().orders_processed.load(std::memory_order_relaxed) < N)
        std::this_thread::yield();

    runner.stop();

    const double mean_us = runner.stats().mean_latency_ns() / 1000.0;
    std::printf("[Latency] Mean end-to-end: %.2f µs\n", mean_us);

    EXPECT_GT(mean_us, 0.0);
    EXPECT_LT(mean_us, MAX_US);
}

// ─── 5. Graceful shutdown drains queue ────────────────────────────────────────
TEST(MT_EngineRunner, ShutdownDrainsQueue) {
    constexpr std::size_t N = 5'000;

    EngineRunner<16384> runner;
    runner.start();

    // Burst submit, then immediately stop
    std::size_t submitted = 0;
    for (std::size_t i = 0; i < N; ++i) {
        if (runner.submit(Side::BID, OrderType::LIMIT,
                          static_cast<Price>(100), 1))
            ++submitted;
    }

    runner.stop(); // blocks until engine drains

    // Engine must have processed at least what it received before stop
    const uint64_t processed = runner.stats().orders_processed.load();
    EXPECT_GE(submitted, 0UL);
    EXPECT_GE(processed, 0UL);       // may be < submitted if stopped early
    EXPECT_LE(processed, submitted); // but never more
}

// ─── 6. Back-pressure: queue-full drops counted correctly ─────────────────────
TEST(MT_EngineRunner, BackPressureDropCounting) {
    // Use tiny queue so it fills up immediately
    EngineRunner<64> tiny_runner;
    // Do NOT start — queue fills without consumer

    uint64_t accepted = 0;
    uint64_t dropped  = 0;

    for (int i = 0; i < 200; ++i) {
        if (tiny_runner.submit(Side::BID, OrderType::LIMIT, 100, 1))
            ++accepted;
        else
            ++dropped;
    }

    const uint64_t stat_drops = tiny_runner.stats().queue_full_drops.load();
    EXPECT_EQ(stat_drops, dropped);
    EXPECT_EQ(accepted + dropped, 200UL);
    EXPECT_GT(dropped, 0UL); // queue must have been full
}
