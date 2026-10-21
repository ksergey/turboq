cmake_minimum_required(VERSION 3.24)

project(turboq CXX)

option(turboq_SANITIZER "Build with sanitizer" OFF)
option(turboq_TSAN "Build with ThreadSanitizer" OFF)
option(turboq_BUILD_TESTS "Build tests (*_test.cpp files, linked against doctest)" ON)
option(turboq_PYTHON "Build Python bindings (requires pybind11, fetched via CPM)" OFF)
option(turboq_TOOLS "Build tools (requires cxxopts, fetched via CPM)" ON)

include(cmake/TurboQHelpers.cmake)
include(cmake/GetCPM.cmake)

if (turboq_BUILD_TESTS)
    CPMAddPackage("gh:doctest/doctest@2.5.2")
    enable_testing()
endif()

add_subdirectory(code)

if (turboq_TOOLS)
    add_subdirectory(tools)
endif()

if (turboq_PYTHON)
    add_subdirectory(python)
endif()
