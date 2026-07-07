#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// src/engine_runner.h  —  Multi-threaded HFT Engine Runner
//
// Architecture (3-thread pipeline):
//
//   ┌──────────────┐   order_queue (SPSC)   ┌───────────────┐
//   │  Producer    │ ───────────────────────▶│ Engine thread │
//   │  (external)  │                         │ MatchingEngine│
//   └──────────────┘                         └───────┬───────┘
//                                                    │ trade_queue (SPSC)
//                                            ┌───────▼───────┐
//                                            │Reporter thread│
//                                            │  (stats/log)  │
//                                            └───────────────┘
//
// Thread-safety contract:
//  * submit()  — producer thread only
//  * stats()   — any thread (atomic reads)
//  * start()/stop() — owner thread only
//  * MatchingEngine — engine thread only (single-threaded hot path)
//
// Shutdown: std::jthread stop_token → graceful drain → join
// ─────────────────────────────────────────────────────────────────────────────
#include "matching_engine.h"
#include "order_types.h"
#include "ring_buffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <thread>

// ─── OrderRequest: timestamped work item for the pipeline ────────────────────
struct OrderRequest {
    Side      side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price     price{0};
    Qty       qty{0};
    /// Nanoseconds since epoch at enqueue — used for end-to-end latency
    int64_t   enqueue_ns{0};
};

// ─── EngineStats: all counters are atomic for wait-free reading ───────────────
struct EngineStats {
    std::atomic<uint64_t> orders_submitted{0};   ///< pushed into queue
    std::atomic<uint64_t> orders_processed{0};   ///< consumed by engine
    std::atomic<uint64_t> orders_rejected{0};    ///< pool full / qty==0
    std::atomic<uint64_t> trades_generated{0};   ///< execution reports
    std::atomic<uint64_t> queue_full_drops{0};   ///< producer back-pressure

    /// Sum of (process_ns - enqueue_ns) for all processed orders
    std::atomic<uint64_t> total_latency_ns{0};

    /// Snapshot: mean latency in nanoseconds
    [[nodiscard]] double mean_latency_ns() const noexcept {
        const uint64_t proc = orders_processed.load(std::memory_order_relaxed);
        if (proc == 0) return 0.0;
        return static_cast<double>(
            total_latency_ns.load(std::memory_order_relaxed)) / static_cast<double>(proc);
    }

    /// Approximate throughput given elapsed wall-clock seconds
    [[nodiscard]] double throughput_ops(double elapsed_s) const noexcept {
        if (elapsed_s <= 0.0) return 0.0;
        return static_cast<double>(
            orders_processed.load(std::memory_order_relaxed)) / elapsed_s;
    }
};

// ─── EngineRunner ─────────────────────────────────────────────────────────────
template<std::size_t QueueDepth = 65536>
class EngineRunner {
    static_assert((QueueDepth & (QueueDepth - 1)) == 0,
        "QueueDepth must be a power of 2");

public:
    using TradeCallback = std::function<void(const Trade&)>;

    EngineRunner() = default;

    // Non-copyable, non-movable (atomics + jthreads are pinned)
    EngineRunner(const EngineRunner&)            = delete;
    EngineRunner& operator=(const EngineRunner&) = delete;
    EngineRunner(EngineRunner&&)                 = delete;
    EngineRunner& operator=(EngineRunner&&)      = delete;

    ~EngineRunner() { stop(); }

    // ── Configuration (call before start()) ───────────────────────────────
    void on_trade(TradeCallback cb) { m_trade_cb = std::move(cb); }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    void start() {
        m_engine_thread   = std::jthread([this](std::stop_token st){ engine_loop(st); });
        m_reporter_thread = std::jthread([this](std::stop_token st){ reporter_loop(st); });
    }

    void stop() {
        // Request stop: jthreads join automatically on destruction
        m_engine_thread.request_stop();
        m_reporter_thread.request_stop();
    }

    // ── Producer API (producer thread only) ───────────────────────────────

    /// Push an order request into the pipeline.
    /// Returns false if the queue is full (back-pressure signal).
    [[nodiscard]] bool submit(Side side, OrderType type, Price price, Qty qty) noexcept {
        using clock = std::chrono::steady_clock;
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now().time_since_epoch()).count();

        OrderRequest req{side, type, price, qty, now};
        if (!m_order_queue.push(req)) {
            m_stats.queue_full_drops.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_stats.orders_submitted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // ── Observability (any thread) ─────────────────────────────────────────
    [[nodiscard]] const EngineStats& stats() const noexcept { return m_stats; }

private:
    // ── Engine thread: dequeue orders → matching engine → push trades ─────
    void engine_loop(std::stop_token stoken) {
        using clock = std::chrono::steady_clock;

        // Wire matching engine trade output → reporter queue
        m_engine.on_trade([this](const Trade& t) {
            m_trade_queue.push(t); // best-effort; drop if reporter is slow
        });

        while (!stoken.stop_requested() || !m_order_queue.empty()) {
            auto req = m_order_queue.pop();
            if (!req) {
                // Spin with yield to avoid 100% CPU while respecting stop
                std::this_thread::yield();
                continue;
            }

            Order* order = m_engine.submit(req->side, req->type, req->price, req->qty);
            if (!order) {
                m_stats.orders_rejected.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // Measure end-to-end pipeline latency
            const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock::now().time_since_epoch()).count();
            const uint64_t lat = static_cast<uint64_t>(now - req->enqueue_ns);
            m_stats.total_latency_ns.fetch_add(lat, std::memory_order_relaxed);
            m_stats.orders_processed.fetch_add(1, std::memory_order_relaxed);

            m_engine.release(order);
        }

        // Signal reporter that engine is done
        m_engine_done.store(true, std::memory_order_release);
    }

    // ── Reporter thread: drain trade queue → callback ─────────────────────
    void reporter_loop(std::stop_token stoken) {
        while (!stoken.stop_requested()
               || !m_engine_done.load(std::memory_order_acquire)
               || !m_trade_queue.empty())
        {
            auto trade = m_trade_queue.pop();
            if (!trade) {
                std::this_thread::yield();
                continue;
            }
            m_stats.trades_generated.fetch_add(1, std::memory_order_relaxed);
            if (m_trade_cb) m_trade_cb(*trade);
        }
    }

    // ── Member data ───────────────────────────────────────────────────────
    RingBuffer<OrderRequest, QueueDepth> m_order_queue;
    RingBuffer<Trade,        QueueDepth> m_trade_queue;

    MatchingEngine<>  m_engine;
    TradeCallback     m_trade_cb;
    EngineStats       m_stats;

    std::atomic<bool> m_engine_done{false};

    std::jthread m_engine_thread;
    std::jthread m_reporter_thread;
};
