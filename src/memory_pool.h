#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// src/memory_pool.h  —  Fixed-size object pool (std::pmr-compatible)
//
// Design notes:
//  * Pre-allocates N objects at construction — zero heap allocations during
//    trading session (critical for deterministic latency)
//  * Free list stored as intrusive linked list inside the free slots
//  * Not thread-safe by design — each thread owns its own pool
//  * Implements std::pmr::memory_resource for STL container integration
// ─────────────────────────────────────────────────────────────────────────────
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

// ─── FixedPool<T, N> ─────────────────────────────────────────────────────────
// Allocates exactly N objects of type T from a single contiguous block.
// acquire() / release() are O(1), branch-free after pool is initialized.
template<typename T, std::size_t N>
class FixedPool {
    static_assert(N > 0, "Pool size must be positive");
    static_assert(sizeof(T) >= sizeof(void*),
        "T must be at least pointer-sized for free-list storage");
public:
    FixedPool() {
        // Build intrusive free list through the raw storage
        for (std::size_t i = 0; i < N - 1; ++i)
            reinterpret_cast<Node*>(slot(i))->next = reinterpret_cast<Node*>(slot(i + 1));
        reinterpret_cast<Node*>(slot(N - 1))->next = nullptr;
        m_free_head = reinterpret_cast<Node*>(slot(0));
        m_free_count = N;
    }

    // Not copyable or movable — pool owns pre-allocated storage
    FixedPool(const FixedPool&)            = delete;
    FixedPool& operator=(const FixedPool&) = delete;
    FixedPool(FixedPool&&)                 = delete;
    FixedPool& operator=(FixedPool&&)      = delete;

    // Returns nullptr when pool is exhausted (caller must handle)
    [[nodiscard]] T* acquire() noexcept {
        if (!m_free_head) return nullptr;
        Node* node    = m_free_head;
        m_free_head   = node->next;
        --m_free_count;
        return reinterpret_cast<T*>(node);
    }

    // Construct in place; returns nullptr on exhaustion
    template<typename... Args>
    [[nodiscard]] T* construct(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        T* ptr = acquire();
        if (!ptr) return nullptr;
        return ::new (ptr) T(std::forward<Args>(args)...);
    }

    // Destroy + return slot to free list
    void destroy(T* ptr) noexcept {
        if (!ptr) return;
        ptr->~T();
        release(ptr);
    }

    // Return raw slot to free list without destruction
    void release(T* ptr) noexcept {
        if (!ptr) return;
        Node* node    = reinterpret_cast<Node*>(ptr);
        node->next    = m_free_head;
        m_free_head   = node;
        ++m_free_count;
    }

    [[nodiscard]] std::size_t capacity()   const noexcept { return N; }
    [[nodiscard]] std::size_t free_count() const noexcept { return m_free_count; }
    [[nodiscard]] std::size_t used_count() const noexcept { return N - m_free_count; }
    [[nodiscard]] bool        full()       const noexcept { return m_free_count == 0; }
    [[nodiscard]] bool        empty()      const noexcept { return m_free_count == N; }

private:
    struct Node { Node* next; };

    // Aligned storage — one slot per object
    alignas(alignof(T)) std::byte m_storage[N * sizeof(T)];

    Node*       m_free_head{nullptr};
    std::size_t m_free_count{0};

    std::byte* slot(std::size_t i) noexcept {
        return m_storage + i * sizeof(T);
    }
};

// ─── PoolResource ─────────────────────────────────────────────────────────────
// std::pmr::memory_resource backed by a contiguous buffer.
// Useful with std::pmr::map, std::pmr::vector, etc.
// Strategy: monotonic bump allocator with alignment support.
// No deallocation (orders live until session end or cancel).
class PoolResource : public std::pmr::memory_resource {
public:
    explicit PoolResource(std::size_t capacity_bytes)
        : m_buffer(capacity_bytes), m_offset(0)
    {
        if (capacity_bytes == 0)
            throw std::invalid_argument("PoolResource: capacity must be > 0");
    }

    [[nodiscard]] std::size_t bytes_used()      const noexcept { return m_offset; }
    [[nodiscard]] std::size_t bytes_remaining() const noexcept {
        return m_buffer.size() - m_offset;
    }

    void reset() noexcept { m_offset = 0; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // Align up m_offset
        const std::size_t aligned = (m_offset + alignment - 1) & ~(alignment - 1);
        if (aligned + bytes > m_buffer.size())
            throw std::bad_alloc{};
        m_offset = aligned + bytes;
        return static_cast<void*>(m_buffer.data() + aligned);
    }

    void do_deallocate(void*, std::size_t, std::size_t) noexcept override {
        // Monotonic allocator: no individual deallocation
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other)
        const noexcept override
    {
        return this == &other;
    }

    std::vector<std::byte> m_buffer;
    std::size_t            m_offset;
};
