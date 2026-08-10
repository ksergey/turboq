CPMAddPackage("gh:jarro2783/cxxopts@3.3.1")

add_executable(multicast_producer multicast_producer.cpp)
target_compile_options(multicast_producer
    PRIVATE -Wall -Wextra -Wattributes -Wpedantic -Wstrict-aliasing -Wcast-align -g)
target_compile_definitions(multicast_producer
    PRIVATE -DCXXOPTS_NO_RTTI)
target_link_libraries(multicast_producer
    PRIVATE cxxopts::cxxopts turboq::turboq)

add_executable(multicast_consumer multicast_consumer.cpp)
target_compile_options(multicast_consumer
    PRIVATE -Wall -Wextra -Wattributes -Wpedantic -Wstrict-aliasing -Wcast-align -g)
target_compile_definitions(multicast_consumer
    PRIVATE -DCXXOPTS_NO_RTTI)
target_link_libraries(multicast_consumer
    PRIVATE cxxopts::cxxopts turboq::turboq)

add_executable(latency_bench latency_bench.cpp)
target_compile_options(latency_bench
    PRIVATE -Wall -Wextra -Wattributes -Wpedantic -Wstrict-aliasing -Wcast-align -g)
target_compile_definitions(latency_bench
    PRIVATE -DCXXOPTS_NO_RTTI)
target_link_libraries(latency_bench
    PRIVATE cxxopts::cxxopts turboq::turboq)
