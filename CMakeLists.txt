cmake_minimum_required(VERSION 3.24)

project(turboq CXX)

option(TURBOQ_PYTHON "Build python binding" ON)
option(TURBOQ_SANITIZER "Build with sanitizer" OFF)

include(cmake/TurboQHelpers.cmake)

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

if(TURBOQ_PYTHON)
  if(NOT COMMAND nanobind_add_module)
    add_subdirectory(deps/nanobind)
  endif()

  add_subdirectory(python)
endif()
