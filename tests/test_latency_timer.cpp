#include "latency_timer.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

TEST(LatencyTimer, ElapsedPositive) {
    ScopedTimer t;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    EXPECT_GT(t.elapsed_ns(), 0.0);
}

TEST(LatencyTimer, ElapsedGrowsOverTime) {
    ScopedTimer t;
    const double t1 = t.elapsed_ns();
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    const double t2 = t.elapsed_ns();
    EXPECT_GT(t2, t1);
}

TEST(LatencyTimer, ResetWorks) {
    ScopedTimer t;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    t.reset();
    const double after_reset = t.elapsed_ns();
    EXPECT_LT(after_reset, 100'000.0);  // should be much less than 100 us
}

TEST(LatencyTimer, CyclesPositive) {
    ScopedTimer t;
    EXPECT_GT(t.elapsed_cycles(), 0u);
}

TEST(LatencyStats, RecordAndQuery) {
    LatencyStats s;
    s.record(10.0);
    s.record(20.0);
    s.record(30.0);

    EXPECT_EQ(s.count(), 3u);
    EXPECT_DOUBLE_EQ(s.min_ns(), 10.0);
    EXPECT_DOUBLE_EQ(s.max_ns(), 30.0);
    EXPECT_DOUBLE_EQ(s.mean_ns(), 20.0);
}

TEST(LatencyStats, Reset) {
    LatencyStats s;
    s.record(50.0);
    s.reset();
    EXPECT_EQ(s.count(), 0u);
    EXPECT_DOUBLE_EQ(s.mean_ns(), 0.0);
}

TEST(LatencyStats, ReportNotEmpty) {
    LatencyStats s;
    s.record(42.5);
    EXPECT_FALSE(s.report().empty());
}

TEST(RdtscCalibrator, InvalidSampleMs) {
    EXPECT_THROW(RdtscCalibrator::calibrate(0),     std::invalid_argument);
    EXPECT_THROW(RdtscCalibrator::calibrate(-1),    std::invalid_argument);
    EXPECT_THROW(RdtscCalibrator::calibrate(99999), std::invalid_argument);
}

TEST(RdtscCalibrator, CyclesPerNsPositive) {
    EXPECT_GT(RdtscCalibrator::cycles_per_ns(), 0.0);
}
