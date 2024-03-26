cmake_minimum_required(VERSION 3.24)

project(turboq CXX)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TurboQHelpers)

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

enable_testing()

add_subdirectory(code)
add_subdirectory(examples)
