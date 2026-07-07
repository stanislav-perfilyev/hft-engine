#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// src/ring_buffer.h  —  Lock-free SPSC ring buffer (cache-line optimized)
//
// Design notes:
//  * Single-Producer Single-Consumer — no CAS, only load/store atomics
//  * alignas(64) on head/tail: prevents false sharing between cores
//  * Power-of-2 capacity: bitmask replaces modulo (branch-free)
//  * "Waste one slot" full detection: head-tail == Capacity-1
//  * T requirements: nothrow_move_constructible (no default-constructible needed)
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

template<typename T, std::size_t Capacity>
    requires (std::is_nothrow_move_constructible_v<T>)
class RingBuffer {
    static_assert(Capacity >= 2,      "Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of 2");

    static constexpr std::size_t kMask = Capacity - 1;

public:
    RingBuffer() = default;
    ~RingBuffer() = default;

    // Not copyable or movable — atomics and buffer are fixed in place
    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&)                 = delete;
    RingBuffer& operator=(RingBuffer&&)      = delete;

    // ── Producer API (call from producer thread only) ──────────────────────

    [[nodiscard]] bool push(const T& item)
        noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        return push_impl(item);
    }

    [[nodiscard]] bool push(T&& item) noexcept {
        return push_impl(std::move(item));
    }

    // ── Consumer API (call from consumer thread only) ──────────────────────

    // Returns false if empty. Moves item out of buffer.
    [[nodiscard]] bool pop(T& out) noexcept {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire))
            return false;  // empty
        out = std::move(m_buffer[tail & kMask]);
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Returns std::nullopt if empty. Does not require default-constructible T.
    [[nodiscard]] std::optional<T> pop() noexcept {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire))
            return std::nullopt;
        auto result = std::make_optional<T>(std::move(m_buffer[tail & kMask]));
        m_tail.store(tail + 1, std::memory_order_release);
        return result;
    }

    // ── Non-mutating queries (approximate — may be stale) ─────────────────

    [[nodiscard]] bool empty() const noexcept {
        return m_head.load(std::memory_order_acquire)
            == m_tail.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        const std::size_t head = m_head.load(std::memory_order_acquire);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        return (head - tail) == Capacity - 1;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t head = m_head.load(std::memory_order_acquire);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        return head - tail;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    template<typename U>
    [[nodiscard]] bool push_impl(U&& item) noexcept {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        if (head - m_tail.load(std::memory_order_acquire) == Capacity - 1)
            return false;  // full — waste-one-slot sentinel
        m_buffer[head & kMask] = std::forward<U>(item);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Producer-owned: head lives on its own cache line
    alignas(64) std::atomic<std::size_t> m_head{0};
    // Consumer-owned: tail lives on its own cache line
    alignas(64) std::atomic<std::size_t> m_tail{0};
    // Buffer: separate from head/tail to avoid false sharing
    alignas(64) T m_buffer[Capacity];
};
