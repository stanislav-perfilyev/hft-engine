#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// src/order_book.h  —  Price-level Order Book (L2 + L3)
//
// Design:
//  * Bids: std::map<Price, Level, std::greater<>> — highest price first
//  * Asks: std::map<Price, Level, std::less<>>    — lowest price first
//  * Level: ordered list of orders at a price (FIFO priority)
//  * O(log P) add/cancel where P = distinct price levels (small in practice)
//  * PoolResource supplied externally — zero heap alloc during hot path
//
// Bug fix (2026-07-02): BidMap/AskMap differ in comparator type — ternary
// operator cannot unify them. Replaced ternary with private template helpers
// dispatched via if/else on order->side.
// ─────────────────────────────────────────────────────────────────────────────
#include "order_types.h"
#include "memory_pool.h"
#include <functional>
#include <map>
#include <memory_resource>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// ─── Level ────────────────────────────────────────────────────────────────────
// All orders at one price, in FIFO order.
struct Level {
    Price                            price{0};
    Qty                              total_qty{0};
    std::pmr::vector<Order*>         orders;

    explicit Level(Price p, std::pmr::memory_resource* mr)
        : price(p), total_qty(0), orders(mr) {}
};

// ─── OrderBook ────────────────────────────────────────────────────────────────
class OrderBook {
public:
    using BidMap = std::pmr::map<Price, Level, std::greater<Price>>;
    using AskMap = std::pmr::map<Price, Level, std::less<Price>>;

    // mr: upstream allocator (PoolResource recommended for HFT)
    explicit OrderBook(std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : m_mr(mr)
        , m_bids(std::pmr::polymorphic_allocator<std::pair<const Price, Level>>{mr})
        , m_asks(std::pmr::polymorphic_allocator<std::pair<const Price, Level>>{mr})
        , m_order_index(std::pmr::polymorphic_allocator<std::pair<const OrderId, Order*>>{mr})
    {}

    // Returns pointer to added order (owned by pool / caller), throws on duplicate id
    Order* add_order(Order* order) {
        if (!order)
            throw std::invalid_argument("add_order: null order");
        if (order->id == kInvalidOrderId)
            throw std::invalid_argument("add_order: id=0 is reserved (kInvalidOrderId)");
        if (m_order_index.count(order->id))
            throw std::invalid_argument("add_order: duplicate order id");

        // BidMap and AskMap have different comparator types — cannot unify via
        // ternary. Dispatch explicitly to the appropriate typed map.
        if (order->side == Side::BID)
            add_to_map(m_bids, order);
        else
            add_to_map(m_asks, order);

        m_order_index[order->id] = order;
        return order;
    }

    // Returns true if cancelled, false if not found
    [[nodiscard]] bool cancel_order(OrderId id) noexcept {
        auto idx_it = m_order_index.find(id);
        if (idx_it == m_order_index.end()) return false;

        Order* order = idx_it->second;
        order->status = OrderStatus::CANCELLED;

        if (order->side == Side::BID)
            cancel_from_map(m_bids, order);
        else
            cancel_from_map(m_asks, order);

        m_order_index.erase(idx_it);
        return true;
    }

    // ── Queries ───────────────────────────────────────────────────────────

    [[nodiscard]] Order* find(OrderId id) const noexcept {
        auto it = m_order_index.find(id);
        return (it != m_order_index.end()) ? it->second : nullptr;
    }

    // Best bid (highest price), nullptr if no bids
    [[nodiscard]] const Level* best_bid() const noexcept {
        return m_bids.empty() ? nullptr : &m_bids.begin()->second;
    }

    // Best ask (lowest price), nullptr if no asks
    [[nodiscard]] const Level* best_ask() const noexcept {
        return m_asks.empty() ? nullptr : &m_asks.begin()->second;
    }

    [[nodiscard]] Price spread() const noexcept {
        if (!best_bid() || !best_ask()) return 0;
        return best_ask()->price - best_bid()->price;
    }

    [[nodiscard]] bool empty() const noexcept {
        return m_bids.empty() && m_asks.empty();
    }

    [[nodiscard]] std::size_t order_count() const noexcept {
        return m_order_index.size();
    }

    // L2 snapshot: top N levels each side
    struct L2Snapshot {
        std::vector<PriceLevel> bids;
        std::vector<PriceLevel> asks;
    };

    [[nodiscard]] L2Snapshot snapshot(std::size_t depth = 5) const {
        L2Snapshot snap;
        std::size_t n = 0;
        for (const auto& [price, level] : m_bids) {
            if (n++ >= depth) break;
            snap.bids.push_back({price, level.total_qty,
                static_cast<int>(level.orders.size())});
        }
        n = 0;
        for (const auto& [price, level] : m_asks) {
            if (n++ >= depth) break;
            snap.asks.push_back({price, level.total_qty,
                static_cast<int>(level.orders.size())});
        }
        return snap;
    }

    // Internal access for MatchingEngine
    BidMap& bids() noexcept { return m_bids; }
    AskMap& asks() noexcept { return m_asks; }
    const BidMap& bids() const noexcept { return m_bids; }
    const AskMap& asks() const noexcept { return m_asks; }

    void remove_from_index(OrderId id) noexcept { m_order_index.erase(id); }

private:
    std::pmr::memory_resource* m_mr;
    BidMap m_bids;
    AskMap m_asks;
    std::pmr::unordered_map<OrderId, Order*> m_order_index;

    // Template helpers avoid duplicating add/cancel logic for two distinct
    // map types (BidMap uses std::greater, AskMap uses std::less).
    template<typename Map>
    void add_to_map(Map& side_map, Order* order) {
        auto it = side_map.find(order->price);
        if (it == side_map.end()) {
            auto [ins, ok] = side_map.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(order->price),
                std::forward_as_tuple(order->price, m_mr)
            );
            it = ins;
        }
        it->second.total_qty += order->remaining();  // use remaining (partial fills may have qty < order->qty)
        it->second.orders.push_back(order);
    }

    template<typename Map>
    void cancel_from_map(Map& side_map, Order* order) noexcept {
        auto level_it = side_map.find(order->price);
        if (level_it == side_map.end()) return;
        auto& v = level_it->second.orders;
        for (auto oit = v.begin(); oit != v.end(); ++oit) {
            if ((*oit)->id == order->id) {
                level_it->second.total_qty -= order->remaining();
                v.erase(oit);
                break;
            }
        }
        if (v.empty()) side_map.erase(level_it);
    }
};
