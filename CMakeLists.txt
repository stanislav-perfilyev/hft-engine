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

# ── Compiler flags ────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX /permissive- /arch:AVX2)
else()
    add_compile_options(-Wall -Wextra -Werror -pedantic -march=native)
    # Enable RDTSC
    add_compile_options(-msse2)
endif()

# ── Interface library (header-only) ───────────────────────────────────────────
add_library(hft_headers INTERFACE)
target_include_directories(hft_headers INTERFACE ${CMAKE_SOURCE_DIR}/src)

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
FetchContent_MakeAvailable(benchmark)

add_executable(hft_bench
    benchmarks/bench_matching_engine.cpp
    benchmarks/bench_ring_buffer.cpp
)
target_link_libraries(hft_bench PRIVATE hft_headers benchmark::benchmark_main)
