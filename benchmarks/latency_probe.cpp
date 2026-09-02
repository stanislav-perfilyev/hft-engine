// benchmarks/latency_probe.cpp
//
// Standalone sanity check for the README's "~120 ns single match round-trip"
// claim, using the engine's OWN internal RDTSC timer (LatencyStats inside
// MatchingEngine::match()) instead of Google Benchmark's harness.
//
// Why this exists: BM_SingleMatchLatency (bench_matching_engine.cpp) times
// TWO submit() calls (a resting ASK + a matching BID) plus Google Benchmark's
// own per-iteration loop overhead — a much wider scope than "just the match".
// The engine's internal ScopedTimer, by contrast, wraps ONLY match_against()
// inside match() — the same narrow scope the README table is describing.
// This probe reads that internal counter directly, so it's an apples-to-apples
// comparison on THIS machine, independent of Google Benchmark's overhead.
//
// No CMake test/bench dependencies — just the header-only engine.

#include "matching_engine.h"
#include <cstdio>
#include <memory>

int main() {
    // Pay the ~100ms RDTSC calibration cost once, upfront, outside anything we measure.
    RdtscCalibrator::cycles_per_ns();

    // Heap-allocate: MatchingEngine<65536> embeds a 4 MiB FixedPool inline —
    // same reason bench_matching_engine.cpp uses make_unique instead of a
    // stack local (see today's stack-overflow fix).
    auto engine = std::make_unique<MatchingEngine<65536>>();

    constexpr int kWarmup     = 1'000;
    constexpr int kIterations = 200'000;

    auto run_batch = [&](int n) {
        for (int i = 0; i < n; ++i) {
            // Rest an ASK, then send a matching BID at the same price/qty —
            // same shape as BM_SingleMatchLatency, but here every call to
            // submit() (and therefore every call to match() inside it) feeds
            // the engine's own internal LatencyStats via ScopedTimer/RDTSC.
            (void)engine->submit(Side::ASK, OrderType::LIMIT, 10000, 1);
            auto* bid = engine->submit(Side::BID, OrderType::LIMIT, 10000, 1);
            // Taker (bid) fully fills -> FILLED -> not on the book -> we own it, must release.
            // Maker (ask) fill is released internally by the engine automatically.
            if (bid && !bid->is_active()) engine->release(bid);
        }
    };

    run_batch(kWarmup);      // first-touch page faults, branch predictor warm-up
    run_batch(kIterations);  // steady state — warm-up iterations are included in
                              // the stats below (no public reset() on match_latency()),
                              // but at 1k/201k they barely move the mean; min_ns is
                              // the cleanest steady-state number, max_ns shows the
                              // cold-start outlier — itself a fair tail-latency point.

    const auto& stats = engine->match_latency();
    std::printf(
        "Internal RDTSC match_latency() over %llu match() calls "
        "(2 per iteration x %d iterations, incl. %d warm-up):\n%s\n",
        static_cast<unsigned long long>(stats.count()), kIterations, kWarmup,
        stats.report().c_str());

    return 0;
}
