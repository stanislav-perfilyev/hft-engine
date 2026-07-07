#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// src/latency_timer.h  —  RDTSC-based nanosecond latency measurement
//
// Design notes:
//  * RDTSC is the gold standard for HFT latency measurement: ~1 ns granularity
//  * __rdtsc() is available on MSVC/GCC/Clang for x86-64
//  * Calibration: measure wall-clock over ~100 ms to derive cycles-per-ns
//  * LatencyStats: tracks min/max/mean; avoids heap allocation
// ─────────────────────────────────────────────────────────────────────────────
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <atomic>

#if defined(_MSC_VER)
#  include <intrin.h>
#  define HFT_RDTSC() __rdtsc()
#elif defined(__GNUC__) || defined(__clang__)
#  include <x86intrin.h>
#  define HFT_RDTSC() __rdtsc()
#else
#  error "RDTSC not supported on this platform"
#endif

// ─── Calibrator ──────────────────────────────────────────────────────────────
// Measures CPU cycles per nanosecond once at startup.
// Not thread-safe by design — call from a single thread before using timers.
class RdtscCalibrator {
public:
    // Calibrate over ~sample_ms milliseconds (100 ms default for ±0.1% accuracy).
    static double calibrate(int sample_ms = 100) {
        if (sample_ms <= 0 || sample_ms > 10000)
            throw std::invalid_argument("sample_ms must be 1..10000");

        const auto wall_start = std::chrono::steady_clock::now();
        const std::uint64_t tsc_start = HFT_RDTSC();

        const auto target = std::chrono::milliseconds(sample_ms);
        while (std::chrono::steady_clock::now() - wall_start < target)
            ; // busy-wait for accurate measurement

        const std::uint64_t tsc_end = HFT_RDTSC();
        const auto wall_end = std::chrono::steady_clock::now();

        const double wall_ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count()
        );
        const double tsc_delta = static_cast<double>(tsc_end - tsc_start);

        if (wall_ns <= 0.0)
            throw std::runtime_error("calibration: wall clock returned zero delta");

        return tsc_delta / wall_ns;  // cycles per nanosecond
    }

    static double cycles_per_ns() {
        // Lazy-initialize once; subsequent calls return cached value.
        static const double kCpn = calibrate();
        return kCpn;
    }
};

// ─── Scoped timer ────────────────────────────────────────────────────────────
// Usage:
//   ScopedTimer t;
//   /* ... code ... */
//   double ns = t.elapsed_ns();
class ScopedTimer {
public:
    ScopedTimer() noexcept : m_start(HFT_RDTSC()) {}

    [[nodiscard]] double elapsed_ns() const noexcept {
        const std::uint64_t delta = HFT_RDTSC() - m_start;
        return static_cast<double>(delta) / RdtscCalibrator::cycles_per_ns();
    }

    [[nodiscard]] std::uint64_t elapsed_cycles() const noexcept {
        return HFT_RDTSC() - m_start;
    }

    void reset() noexcept { m_start = HFT_RDTSC(); }

private:
    std::uint64_t m_start;
};

// ─── Latency statistics ───────────────────────────────────────────────────────
// Accumulates min/max/mean without heap allocation.
// Not thread-safe — use one instance per thread, merge externally if needed.
class LatencyStats {
public:
    void record(double ns) noexcept {
        ++m_count;
        m_sum_ns  += ns;
        if (ns < m_min_ns) m_min_ns = ns;
        if (ns > m_max_ns) m_max_ns = ns;
    }

    [[nodiscard]] std::uint64_t count()   const noexcept { return m_count; }
    [[nodiscard]] double        min_ns()  const noexcept { return m_min_ns; }
    [[nodiscard]] double        max_ns()  const noexcept { return m_max_ns; }
    [[nodiscard]] double        mean_ns() const noexcept {
        return m_count > 0 ? m_sum_ns / static_cast<double>(m_count) : 0.0;
    }

    void reset() noexcept {
        m_count  = 0;
        m_sum_ns = 0.0;
        m_min_ns = std::numeric_limits<double>::max();
        m_max_ns = 0.0;
    }

    [[nodiscard]] std::string report() const {
        return "count=" + std::to_string(m_count)
             + " min="  + fmt(m_min_ns)  + " ns"
             + " mean=" + fmt(mean_ns())  + " ns"
             + " max="  + fmt(m_max_ns)  + " ns";
    }

private:
    std::uint64_t m_count{0};
    double        m_sum_ns{0.0};
    double        m_min_ns{std::numeric_limits<double>::max()};
    double        m_max_ns{0.0};

    [[nodiscard]] static std::string fmt(double v) {
        // Format to 1 decimal place without printf/format dependencies
        const long long i = static_cast<long long>(v);
        const int frac = static_cast<int>((v - static_cast<double>(i)) * 10.0);
        return std::to_string(i) + "." + std::to_string(frac);
    }
};
