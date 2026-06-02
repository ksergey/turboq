include(FetchContent)

FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
    SYSTEM EXCLUDE_FROM_ALL OVERRIDE_FIND_PACKAGE)
set(BENCHMARK_ENABLE_GTEST_TESTS FALSE)
set(BENCHMARK_ENABLE_ASSEMBLY_TESTS FALSE)
FetchContent_MakeAvailable(benchmark)
