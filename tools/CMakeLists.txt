CPMAddPackage("gh:jarro2783/cxxopts@3.3.1")

add_executable(producer producer.cpp)
target_compile_options(producer
    PRIVATE -Wall -Wextra -Wattributes -Wpedantic -Wstrict-aliasing -Wcast-align -g)
target_compile_definitions(producer
    PRIVATE -DCXXOPTS_NO_RTTI)
target_link_libraries(producer
    PRIVATE cxxopts::cxxopts turboq::turboq)

add_executable(consumer consumer.cpp)
target_compile_options(consumer
    PRIVATE -Wall -Wextra -Wattributes -Wpedantic -Wstrict-aliasing -Wcast-align -g)
target_compile_definitions(consumer
    PRIVATE -DCXXOPTS_NO_RTTI)
target_link_libraries(consumer
    PRIVATE cxxopts::cxxopts turboq::turboq)
