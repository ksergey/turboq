cmake_minimum_required(VERSION 3.24)

project(turboq CXX)

option(turboq_SANITIZER "Build with sanitizer" OFF)

include(cmake/TurboQHelpers.cmake)
include(cmake/GetCPM.cmake)

CPMAddPackage(URI "gh:google/benchmark@1.9.5" OPTIONS "BENCHMARK_ENABLE_GTEST_TESTS OFF; BENCHMARK_ENABLE_TESTING OFF; BENCHMARK_ENABLE_INSTALL OFF")
CPMAddPackage("gh:doctest/doctest@2.5.2")
CPMAddPackage("gh:jarro2783/cxxopts@3.3.1")

enable_testing()

add_subdirectory(code)
add_subdirectory(examples)
