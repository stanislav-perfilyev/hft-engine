cmake_minimum_required(VERSION 3.20)
project(hft_engine CXX)

# ── ccache (faster rebuilds) ──────────────────────────────────────────────────
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND NOT MSVC)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler flags — scoped to OUR OWN targets only (applied below via
#    target_compile_options), NEVER globally via add_compile_options().
#    A directory-scope add_compile_options() here would leak into every
#    FetchContent'd third-party target (googletest/benchmark) added later in
#    this file, turning unrelated upstream warnings into hard build failures.
#    That's exactly what broke the benchmark build: google/benchmark's own
#    sysinfo.cc has an incomplete switch over CacheUnknown, which is harmless
#    upstream but becomes -Werror=switch once our flags leak into it.
if(MSVC)
    set(HFT_WARNING_FLAGS /W4 /WX /permissive- /arch:AVX2)
    # std::getenv is standard C — suppress MSVC non-portable deprecation warning C4996
    set(HFT_COMPILE_DEFS _CRT_SECURE_NO_WARNINGS)
else()
    # -msse2 enables RDTSC
    set(HFT_WARNING_FLAGS -Wall -Wextra -Werror -pedantic -march=native -msse2)
    set(HFT_COMPILE_DEFS "")
endif()

# ── Interface library (header-only) ───────────────────────────────────────────
add_library(hft_headers INTERFACE)
target_include_directories(hft_headers INTERFACE ${CMAKE_SOURCE_DIR}/src)

# ── Latency probe (standalone, no gtest/benchmark deps — builds instantly) ────
# Reads the engine's own internal RDTSC LatencyStats directly, to sanity-check
# the README's "~120 ns single match round-trip" figure against Google
# Benchmark's wider-scope BM_SingleMatchLatency on this same machine.
add_executable(latency_probe benchmarks/latency_probe.cpp)
target_link_libraries(latency_probe PRIVATE hft_headers)
target_compile_options(latency_probe PRIVATE ${HFT_WARNING_FLAGS})
target_compile_definitions(latency_probe PRIVATE ${HFT_COMPILE_DEFS})

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(hft_tests
    tests/test_order_types.cpp
    tests/test_ring_buffer.cpp
    tests/test_order_book.cpp
    tests/test_matching_engine.cpp
    tests/test_latency_timer.cpp
    tests/test_memory_pool.cpp
    tests/test_mt_engine.cpp
)
target_link_libraries(hft_tests PRIVATE hft_headers GTest::gtest_main
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:pthread>)
target_compile_options(hft_tests PRIVATE ${HFT_WARNING_FLAGS})
target_compile_definitions(hft_tests PRIVATE ${HFT_COMPILE_DEFS})
# Suppress gcc-12 false positive: -Wrestrict fires inside gtest-type-util.h
# string::replace (dead code path). Known upstream googletest issue with gcc-12.
target_compile_options(hft_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wno-restrict>
)
include(GoogleTest)
gtest_discover_tests(hft_tests)

# ── Benchmarks ────────────────────────────────────────────────────────────────
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
# google/benchmark enables its own -Werror by default (BENCHMARK_ENABLE_WERROR),
# which is what actually fails the build (-Werror=switch on CacheUnknown in its
# sysinfo.cc) — vendored code, not ours to fix, so we just don't hold it to our
# warning bar.
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

add_executable(hft_bench
    benchmarks/bench_matching_engine.cpp
    benchmarks/bench_ring_buffer.cpp
)
target_link_libraries(hft_bench PRIVATE hft_headers benchmark::benchmark_main)
target_compile_options(hft_bench PRIVATE ${HFT_WARNING_FLAGS})
target_compile_definitions(hft_bench PRIVATE ${HFT_COMPILE_DEFS})
