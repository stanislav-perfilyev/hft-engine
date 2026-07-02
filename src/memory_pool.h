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
// Strategy: monotonic bump allocator.
//
// Alignment guarantee: the backing buffer is aligned to kMaxAlign bytes, so any
// requested alignment <= kMaxAlign is satisfied correctly.  Alignments larger
// than kMaxAlign are unsupported and will throw std::bad_alloc.
class PoolResource : public std::pmr::memory_resource {
    // Maximum alignment we guarantee (one cache line — covers all HFT types).
    static constexpr std::size_t kMaxAlign = 64;
public:
    explicit PoolResource(std::size_t capacity_bytes)
        : m_capacity(capacity_bytes)
        , m_offset(0)
    {
        if (capacity_bytes == 0)
            throw std::invalid_argument("PoolResource: capacity must be > 0");

        // Over-allocate by (kMaxAlign - 1) bytes so we can always find a
        // kMaxAlign-aligned start inside the raw buffer regardless of where
        // the heap places m_raw.data().
        m_raw.resize(capacity_bytes + kMaxAlign - 1);

        const uintptr_t raw_addr     = reinterpret_cast<uintptr_t>(m_raw.data());
        const uintptr_t aligned_addr = (raw_addr + kMaxAlign - 1)
                                       & ~(uintptr_t{kMaxAlign - 1});
        m_start = reinterpret_cast<std::byte*>(aligned_addr);
    }

    [[nodiscard]] std::size_t bytes_used()      const noexcept { return m_offset; }
    [[nodiscard]] std::size_t bytes_remaining() const noexcept {
        return m_capacity - m_offset;
    }

    void reset() noexcept { m_offset = 0; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (alignment > kMaxAlign)
            throw std::bad_alloc{};  // unsupported super-alignment

        // Align up m_offset within the kMaxAlign-aligned buffer.
        // Because m_start is already kMaxAlign-aligned, any alignment <= kMaxAlign
        // is satisfied by the standard mask calculation below.
        const std::size_t aligned = (m_offset + alignment - 1) & ~(alignment - 1);
        if (aligned + bytes > m_capacity)
            throw std::bad_alloc{};
        m_offset = aligned + bytes;
        return static_cast<void*>(m_start + aligned);
    }

    void do_deallocate(void*, std::size_t, std::size_t) noexcept override {
        // Monotonic allocator: no individual deallocation
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other)
        const noexcept override
    {
        return this == &other;
    }

    std::vector<std::byte> m_raw;       // over-allocated raw storage
    std::byte*  m_start{nullptr};       // kMaxAlign-aligned start within m_raw
    std::size_t m_capacity;             // usable bytes
    std::size_t m_offset;
};
