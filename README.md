# hft-engine

A C++20 high-frequency trading engine demonstrating low-latency systems design: lock-free data structures, cache-optimised order book, FIFO matching engine, and sub-microsecond RDTSC latency measurement.

[![CI](https://github.com/stasperfiliyev/hft-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/stasperfiliyev/hft-engine/actions/workflows/ci.yml)

---

## Architecture

```
src/
├── order_types.h        — Core types: Order (64-byte cache line), Trade, PriceLevel
├── ring_buffer.h        — Lock-free SPSC ring buffer (alignas(64) false-sharing prevention)
├── memory_pool.h        — FixedPool<T,N> + std::pmr::memory_resource bump allocator
├── latency_timer.h      — RDTSC ScopedTimer + LatencyStats (min/max/mean)
├── order_book.h         — L2/L3 order book (std::pmr::map bid DESC / ask ASC)
└── matching_engine.h    — FIFO price-time priority matching, zero heap alloc in hot path
```

---

## Design Highlights

### 1 — `alignas(64)` Order struct

Each `Order` occupies exactly **one L1 cache line** (64 bytes). This eliminates false sharing when multiple orders are accessed concurrently and maximises cache utilisation in the matching loop.

```cpp
struct alignas(64) Order {
    OrderId   id;            // 8 bytes
    Price     price;         // 8 bytes  (int64 ticks, no float)
    Qty       qty;           // 4 bytes
    Qty       filled_qty;    // 4 bytes
    Timestamp timestamp_ns;  // 8 bytes
    Side      side;          // 1 byte
    OrderType type;          // 1 byte
    OrderStatus status;      // 1 byte
    uint8_t   _pad[...]{};   // padding to 64
};
static_assert(sizeof(Order) == 64);
```

### 2 — Lock-free SPSC Ring Buffer

Single-Producer Single-Consumer queue using only `memory_order_acquire/release` — no CAS, no mutex. `alignas(64)` on `head` and `tail` prevents false sharing between producer and consumer cores.

```
Producer core            Consumer core
──────────────           ──────────────
head (alignas64)         tail (alignas64)
  ↓                        ↓
[  buffer[N]  ]   ←   power-of-2 bitmask wrap
```

Full detection uses the **waste-one-slot** idiom: `head - tail == Capacity - 1` means full.

### 3 — Zero-Heap Matching Engine

`FixedPool<Order, N>` pre-allocates N orders at startup. `acquire()` / `release()` are O(1) via an intrusive free list. During a trading session, no `new` / `delete` is called on the hot path.

`PoolResource` (a `std::pmr::memory_resource` bump allocator) backs the `std::pmr::map` structures inside `OrderBook` — the entire book lives in one contiguous arena.

### 4 — RDTSC Latency Timer

Wall-clock APIs (`std::chrono::steady_clock`) have 50–200 ns overhead from syscalls. RDTSC reads the CPU timestamp counter in ~1 cycle:

```cpp
ScopedTimer t;
engine.submit(Side::BID, OrderType::LIMIT, 10000, 100);
double ns = t.elapsed_ns();   // ≈ 50–300 ns per match
```

`RdtscCalibrator` measures `cycles_per_ns` once at startup over a 100 ms wall-clock window (±0.1% accuracy).

### 5 — FIFO Price-Time Priority

| Priority | Rule |
|----------|------|
| 1st      | Best price (bids: highest; asks: lowest) |
| 2nd      | Earliest timestamp at same price (FIFO) |

`OrderBook` stores bids in `std::pmr::map<Price, Level, std::greater<>>` and asks in `std::pmr::map<Price, Level, std::less<>>`. Each `Level` holds a `std::pmr::vector<Order*>` in arrival order.

---

## Benchmark Results

Representative numbers on Intel Core i7-12700K, GCC 12, `-O3 -march=native`:

| Benchmark | Result |
|-----------|--------|
| Single match round-trip | ~120 ns |
| Match throughput (4096 fills) | ~28 M fills/sec |
| SPSC RingBuffer (100K items) | ~3.8× faster than mutex queue |
| RingBuffer single-thread push+pop | ~8 ns |

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/hft_bench
```

**Requirements:** C++20 compiler (GCC 12+, Clang 15+, MSVC 19.34+), CMake 3.20+.  
GTest and Google Benchmark are fetched automatically via `FetchContent`.

---

## CI Matrix

| Platform | Compiler |
|----------|----------|
| Ubuntu 22.04 | GCC 12 |
| Ubuntu 22.04 | Clang 15 |
| Windows 2022 | MSVC 19 (ilammy/msvc-dev-cmd) |

---

## Concepts Demonstrated

- Lock-free SPSC queue with `memory_order_acquire/release`
- `std::pmr` polymorphic allocators for zero-heap hot paths
- RDTSC nanosecond timing and calibration
- Price-time FIFO matching (L2/L3 order book)
- `alignas(64)` cache-line pinning of hot structs
- `FixedPool<T,N>` intrusive free-list allocator
- `std::jthread` + `std::stop_token` in benchmarks
