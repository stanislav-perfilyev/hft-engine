// benchmarks/bench_matching_engine.cpp
// Measure round-trip latency of the matching engine hot path.
static constexpr int kLargePoolDepth = 65536;
static constexpr int kSmallPoolDepth =     8;
static constexpr int kBasePrice      = 10000;
static constexpr int kDefaultQty     =   100;
static constexpr int kQtyPerLevel    =    10;
static constexpr int kBenchRangeLo   =    64;
static constexpr int kBenchRangeMed  =  4096;
static constexpr int kBenchRangeHi   =  8192;
static constexpr int kMarketRangeHi  =   256;

#include "matching_engine.h"
#include <benchmark/benchmark.h>
#include <memory>

// NOTE: MatchingEngine<kLargePoolDepth> embeds its FixedPool<Order,N> storage
// inline (by design — zero heap allocation on the hot path once the engine
// exists). At kLargePoolDepth=65536 and sizeof(Order)==64, that's a 4 MiB
// object. Constructing it as a plain stack local inside a benchmark loop
// blows the default ~1 MiB thread stack (STATUS_STACK_OVERFLOW). Heap-
// allocate the engine itself once per iteration instead — this only moves
// where the (still one-time, non-hot-path) engine construction lives; the
// engine's own zero-alloc guarantee for submit()/cancel() is untouched.

// ── BM_LimitOrderRest ─────────────────────────────────────────────────────────
// Submit LIMIT orders that do NOT match (no opposite side) — measures pure
// order insertion + index update latency.
static void BM_LimitOrderRest(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    for (auto _ : state) {
        // Construction (4 MiB heap alloc + O(kLargePoolDepth) free-list init)
        // is setup cost, not submit() cost — exclude it, same as the
        // PauseTiming pattern used below in BM_MatchAndFill/BM_MarketOrder.
        // Left inside the timed region, it dwarfs the per-order cost this
        // benchmark is meant to isolate (dominates small-N rows especially).
        state.PauseTiming();
        auto me = std::make_unique<MatchingEngine<kLargePoolDepth>>();
        state.ResumeTiming();

        for (int i = 0; i < n; ++i) {
            // Alternate prices so they don't cross
            (void)me->submit(Side::BID, OrderType::LIMIT, kBasePrice - i, kDefaultQty);
        }
        benchmark::DoNotOptimize(me);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_LimitOrderRest)->Range(kBenchRangeLo, kBenchRangeHi)->Unit(benchmark::kNanosecond);

// ── BM_MatchAndFill ───────────────────────────────────────────────────────────
// Pre-load N asks, then send N matching bids — measures pure match throughput.
static void BM_MatchAndFill(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));

    for (auto _ : state) {
        // Engine construction + book pre-load are setup, not hot path — both
        // excluded from timing.
        state.PauseTiming();
        auto me = std::make_unique<MatchingEngine<kLargePoolDepth>>();
        for (int i = 0; i < n; ++i)
            (void)me->submit(Side::ASK, OrderType::LIMIT, kBasePrice, 1);
        state.ResumeTiming();

        // Hot path: match all N asks
        for (int i = 0; i < n; ++i)
            (void)me->submit(Side::BID, OrderType::LIMIT, kBasePrice, 1);

        benchmark::DoNotOptimize(me);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_MatchAndFill)->Range(kBenchRangeLo, kBenchRangeMed)->Unit(benchmark::kNanosecond);

// ── BM_SingleMatchLatency ─────────────────────────────────────────────────────
// One ask resting, one bid — measures the minimum single-match round-trip.
static void BM_SingleMatchLatency(benchmark::State& state) {
    for (auto _ : state) {
        MatchingEngine<kSmallPoolDepth> me;
        (void)me.submit(Side::ASK, OrderType::LIMIT, kBasePrice, kDefaultQty);
        auto* bid = me.submit(Side::BID, OrderType::LIMIT, kBasePrice, kDefaultQty);
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
        state.PauseTiming();
        auto me = std::make_unique<MatchingEngine<kLargePoolDepth>>();
        for (int i = 0; i < levels; ++i)
            (void)me->submit(Side::ASK, OrderType::LIMIT, kBasePrice + i, kQtyPerLevel);
        state.ResumeTiming();

        // Market buy sweeps all levels
        (void)me->submit(Side::BID, OrderType::MARKET, 0, kQtyPerLevel * levels);
        benchmark::DoNotOptimize(me);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_MarketOrder)->Range(1, kMarketRangeHi)->Unit(benchmark::kNanosecond);

