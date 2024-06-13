cmake_minimum_required(VERSION 3.24)

project(turboq CXX)

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(TurboQMasterProject ON)
else()
  set(TurboQMasterProject OFF)
endif()

include(cmake/TurboQHelpers.cmake)

if(NOT TARGET doctest::doctest_with_main)
  add_subdirectory(deps/doctest)
endif()
if(NOT TARGET benchmark::benchmark_main)
  add_subdirectory(deps/benchmark)
endif()
if(NOT TARGET cxxopts::cxxopts)
  add_subdirectory(deps/cxxopts)
endif()

enable_testing()

add_subdirectory(code)
add_subdirectory(examples)
