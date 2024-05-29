cmake_minimum_required(VERSION 3.24)

project(turboq CXX)

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(TurboQ_MasterProject ON)
else()
  set(TurboQ_MasterProject OFF)
endif()

option(TurboQ_Sanitize "Enable sanitized targets" ${TurboQ_MasterProject})

include(cmake/TurboQHelpers.cmake)
include(cmake/Sanitize.cmake)

if(NOT TARGET fmt::fmt)
  add_subdirectory(deps/fmt)
endif()
if(NOT TARGET Boost::outcome)
  add_subdirectory(deps/boost_outcome)
endif()
if(NOT TARGET Boost::scope_exit)
  add_subdirectory(deps/boost_scope_exit)
endif()
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
