CPMAddPackage("gh:jarro2783/cxxopts@3.3.1")

add_executable(latency_bench latency_bench.cpp)
target_compile_options(latency_bench
    PRIVATE -Wall -Wextra -Wattributes -Wpedantic -Wstrict-aliasing -Wcast-align -g)
target_compile_definitions(latency_bench
    PRIVATE -DCXXOPTS_NO_RTTI)
target_link_libraries(latency_bench
    PRIVATE cxxopts::cxxopts turboq::turboq)
