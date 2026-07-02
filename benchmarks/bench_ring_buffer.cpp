// benchmarks/bench_ring_buffer.cpp
// SPSC RingBuffer throughput vs mutex queue baseline.
#include "ring_buffer.h"
#include <benchmark/benchmark.h>
#include <mutex>
#include <queue>
#include <thread>

// ── SPSC RingBuffer throughput ────────────────────────────────────────────────
static void BM_RingBuffer_SPSC(benchmark::State& state) {
    const long long items = state.range(0);

    for (auto _ : state) {
        RingBuffer<int, 1024> rb;
        int received = 0;

        // Producer inside loop — rule from feedback_error_patterns ## 8
        std::jthread producer([&](std::stop_token) {
            for (long long i = 0; i < items; ++i)
                while (!rb.push(static_cast<int>(i))) std::this_thread::yield();
        });

        long long count = 0;
        while (count < items) {
            if (rb.pop(received)) ++count;
            else std::this_thread::yield();
        }
        benchmark::DoNotOptimize(received);
        benchmark::ClobberMemory();
    }  // producer joins here via RAII

    state.SetItemsProcessed(state.iterations() * items);
}
BENCHMARK(BM_RingBuffer_SPSC)->Arg(100000)->Unit(benchmark::kMillisecond);

// ── Mutex queue baseline ──────────────────────────────────────────────────────
template<typename T>
class MutexQueue {
public:
    void push(T v) { std::lock_guard g(m_mu); m_q.push(v); }
    bool pop(T& out) {
        std::lock_guard g(m_mu);
        if (m_q.empty()) return false;
        out = m_q.front(); m_q.pop();
        return true;
    }
private:
    std::mutex m_mu;
    std::queue<T> m_q;
};

static void BM_MutexQueue_SPSC(benchmark::State& state) {
    const long long items = state.range(0);

    for (auto _ : state) {
        MutexQueue<int> q;
        int received = 0;

        std::jthread producer([&](std::stop_token) {
            for (long long i = 0; i < items; ++i)
                q.push(static_cast<int>(i));
        });

        long long count = 0;
        while (count < items) {
            if (q.pop(received)) ++count;
            else std::this_thread::yield();
        }
        benchmark::DoNotOptimize(received);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * items);
}
BENCHMARK(BM_MutexQueue_SPSC)->Arg(100000)->Unit(benchmark::kMillisecond);

// ── Single-thread push/pop latency ────────────────────────────────────────────
static void BM_RingBuffer_SingleThread(benchmark::State& state) {
    RingBuffer<int, 1024> rb;
    int v = 0;
    for (auto _ : state) {
        rb.push(42);
        rb.pop(v);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_RingBuffer_SingleThread)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
