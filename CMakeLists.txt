cmake_minimum_required(VERSION 3.24)

project(turboq CXX)

option(turboq_SANITIZER "Build with sanitizer" OFF)

include(cmake/TurboQHelpers.cmake)
include(cmake/GetCPM.cmake)

CPMAddPackage("gh:doctest/doctest@2.5.2")

enable_testing()

add_subdirectory(code)
add_subdirectory(tools)
