// benchmarks/bench_matching_engine.cpp
// Measure round-trip latency of the matching engine hot path.
#include "matching_engine.h"
#include <benchmark/benchmark.h>

// ── BM_LimitOrderRest ─────────────────────────────────────────────────────────
// Submit LIMIT orders that do NOT match (no opposite side) — measures pure
// order insertion + index update latency.
static void BM_LimitOrderRest(benchmark::State& state) {
    for (auto _ : state) {
        MatchingEngine<65536> me;
        const int n = static_cast<int>(state.range(0));
        for (int i = 0; i < n; ++i) {
            // Alternate prices so they don't cross
            (void)me.submit(Side::BID, OrderType::LIMIT, 10000 - i, 100);
        }
        benchmark::DoNotOptimize(me);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_LimitOrderRest)->Range(64, 8192)->Unit(benchmark::kNanosecond);

// ── BM_MatchAndFill ───────────────────────────────────────────────────────────
// Pre-load N asks, then send N matching bids — measures pure match throughput.
static void BM_MatchAndFill(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));

    for (auto _ : state) {
        MatchingEngine<65536> me;

        // Setup phase (excluded from timing by benchmark framework)
        state.PauseTiming();
        for (int i = 0; i < n; ++i)
            (void)me.submit(Side::ASK, OrderType::LIMIT, 10000, 1);
        state.ResumeTiming();

        // Hot path: match all N asks
        for (int i = 0; i < n; ++i)
            (void)me.submit(Side::BID, OrderType::LIMIT, 10000, 1);

        benchmark::DoNotOptimize(me);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_MatchAndFill)->Range(64, 4096)->Unit(benchmark::kNanosecond);

// ── BM_SingleMatchLatency ─────────────────────────────────────────────────────
// One ask resting, one bid — measures the minimum single-match round-trip.
static void BM_SingleMatchLatency(benchmark::State& state) {
    for (auto _ : state) {
        MatchingEngine<8> me;
        (void)me.submit(Side::ASK, OrderType::LIMIT, 10000, 100);
        auto* bid = me.submit(Side::BID, OrderType::LIMIT, 10000, 100);
        benchmark::DoNotOptimize(bid);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_SingleMatchLatency)->Unit(benchmark::kNanosecond);

// ── BM_MarketOrder ────────────────────────────────────────────────────────────
// Market order sweeping multiple price levels.
static void BM_MarketOrder(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));

    for (auto _ : state) {
        MatchingEngine<65536> me;

        state.PauseTiming();
        for (int i = 0; i < levels; ++i)
            (void)me.submit(Side::ASK, OrderType::LIMIT, 10000 + i, 10);
        state.ResumeTiming();

        // Market buy sweeps all levels
        (void)me.submit(Side::BID, OrderType::MARKET, 0, 10 * levels);
        benchmark::DoNotOptimize(me);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_MarketOrder)->Range(1, 256)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
