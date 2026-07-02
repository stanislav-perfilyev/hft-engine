#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// src/matching_engine.h  —  FIFO Price-Time Priority Matching Engine
//
// Design:
//  * LIMIT order: rest on book or match against opposite side (price-time FIFO)
//  * MARKET order: match until filled or book exhausted → REJECTED if no fill
//  * Partial fills update filled_qty on both maker and taker orders
//  * Each execution generates a Trade report
//  * FixedPool<Order, N> prevents heap allocation during matching
//  * Not thread-safe — single-threaded hot path; external synchronization if needed
// ─────────────────────────────────────────────────────────────────────────────
#include "order_book.h"
#include "order_types.h"
#include "memory_pool.h"
#include "latency_timer.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

// ─── MatchingEngine ───────────────────────────────────────────────────────────
template<std::size_t MaxOrders = 65536>
class MatchingEngine {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    explicit MatchingEngine(std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : m_book(mr)
        , m_next_id(1)
    {}

    // Register a callback invoked for every execution report
    void on_trade(TradeCallback cb) { m_trade_cb = std::move(cb); }

    // ── Primary entry point ───────────────────────────────────────────────

    // Submit a new order. Returns the allocated Order* (valid until cancel/fill).
    // Returns nullptr if pool exhausted.
    Order* submit(Side side, OrderType type, Price price, Qty qty) {
        if (qty == 0) return nullptr;

        Order* order = m_pool.construct();
        if (!order) return nullptr;

        order->id           = m_next_id++;
        order->side         = side;
        order->type         = type;
        order->price        = price;
        order->qty          = qty;
        order->filled_qty   = 0;
        order->timestamp_ns = now_ns();
        order->status       = OrderStatus::NEW;

        match(order);

        if (order->is_active()) {
            if (type == OrderType::MARKET) {
                // Market orders that can't fully fill → reject remainder
                order->status = OrderStatus::REJECTED;
            } else {
                // LIMIT: rest on book
                m_book.add_order(order);
            }
        }

        return order;
    }

    // Cancel resting order by id. Returns true if found and cancelled.
    bool cancel(OrderId id) {
        Order* order = m_book.find(id);
        if (!order) return false;
        const bool ok = m_book.cancel_order(id);
        if (ok) m_pool.destroy(order);
        return ok;
    }

    [[nodiscard]] const OrderBook&     book()          const noexcept { return m_book; }
    [[nodiscard]] const LatencyStats&  match_latency() const noexcept { return m_latency; }
    [[nodiscard]] std::uint64_t        total_orders()  const noexcept { return m_next_id - 1; }
    [[nodiscard]] std::uint64_t        total_trades()  const noexcept { return m_total_trades; }

private:
    // ── Matching logic ────────────────────────────────────────────────────

    void match(Order* taker) noexcept {
        ScopedTimer timer;

        if (taker->side == Side::BID) {
            match_against(taker, m_book.asks());
        } else {
            match_against(taker, m_book.bids());
        }

        m_latency.record(timer.elapsed_ns());
    }

    template<typename SideMap>
    void match_against(Order* taker, SideMap& opposite) noexcept {
        for (auto level_it = opposite.begin();
             level_it != opposite.end() && taker->is_active(); )
        {
            // Price check
            if (taker->type == OrderType::LIMIT) {
                if (taker->side == Side::BID && level_it->first > taker->price) break;
                if (taker->side == Side::ASK && level_it->first < taker->price) break;
            }

            Level& level = level_it->second;
            auto order_it = level.orders.begin();

            while (order_it != level.orders.end() && taker->is_active()) {
                Order* maker = *order_it;
                const Qty fill_qty = std::min(taker->remaining(), maker->remaining());

                // Execute trade
                Trade trade;
                trade.maker_id    = maker->id;
                trade.taker_id    = taker->id;
                trade.price       = maker->price;
                trade.qty         = fill_qty;
                trade.timestamp_ns = now_ns();

                maker->filled_qty += fill_qty;
                taker->filled_qty += fill_qty;
                level.total_qty   -= fill_qty;

                if (maker->remaining() == 0) {
                    maker->status = OrderStatus::FILLED;
                } else {
                    maker->status = OrderStatus::PARTIAL;
                }

                taker->status = (taker->remaining() == 0)
                    ? OrderStatus::FILLED : OrderStatus::PARTIAL;

                ++m_total_trades;
                if (m_trade_cb) m_trade_cb(trade);

                if (maker->status == OrderStatus::FILLED) {
                    m_book.remove_from_index(maker->id);
                    order_it = level.orders.erase(order_it);
                    m_pool.destroy(maker);
                } else {
                    ++order_it;
                }
            }

            if (level.orders.empty()) {
                level_it = opposite.erase(level_it);
            } else {
                ++level_it;
            }
        }
    }

    static Timestamp now_ns() noexcept {
        return static_cast<Timestamp>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }

    OrderBook             m_book;
    FixedPool<Order, MaxOrders> m_pool;
    TradeCallback         m_trade_cb;
    LatencyStats          m_latency;
    OrderId               m_next_id{1};
    std::uint64_t         m_total_trades{0};
};
