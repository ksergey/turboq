CPMAddPackage("gh:pybind/pybind11@3.1.0")

add_library(_turboq_rtti_override INTERFACE)
target_compile_options(_turboq_rtti_override INTERFACE -frtti)

pybind11_add_module(_turboq MODULE _turboq.cpp)
target_link_libraries(_turboq PRIVATE turboq::turboq _turboq_rtti_override)

# Drop the compiled extension next to the pure-Python wrapper so the result
# under ${CMAKE_CURRENT_BINARY_DIR}/turboq is already an importable package
# (`PYTHONPATH=<build dir>/python python3 -c "import turboq"`).
set_target_properties(_turboq
    PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/turboq")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/turboq/__init__.py"
    "${CMAKE_CURRENT_BINARY_DIR}/turboq/__init__.py"
    COPYONLY)

install(TARGETS _turboq LIBRARY DESTINATION turboq)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/turboq/__init__.py" DESTINATION turboq)

# Register the pytest suite under tests/ with ctest, reusing turboq_BUILD_TESTS (the same option
# that gates the C++ doctest suite in code/CMakeLists.txt) so `-Dturboq_BUILD_TESTS=OFF` turns off
# all tests uniformly. pybind11_add_module() above already located a Python interpreter, but under
# which variable depends on its PYBIND11_FINDPYTHON mode (Python_EXECUTABLE for the modern
# FindPython module vs Python3_EXECUTABLE for the legacy/compatibility one) -- fall back between
# the two rather than assuming.
if (turboq_BUILD_TESTS)
    if (Python_EXECUTABLE)
        set(_turboq_python_executable "${Python_EXECUTABLE}")
    else()
        set(_turboq_python_executable "${Python3_EXECUTABLE}")
    endif()

    execute_process(
        COMMAND "${_turboq_python_executable}" -c "import pytest"
        RESULT_VARIABLE _turboq_pytest_missing
        OUTPUT_QUIET ERROR_QUIET)
    if (_turboq_pytest_missing EQUAL 0)
        add_test(
            NAME turboq-python-test
            COMMAND "${_turboq_python_executable}" -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/tests" -v)
        # The compiled extension lives under the build tree, not the source tree -- tests import
        # `turboq` from there.
        set_tests_properties(turboq-python-test PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}")
    else()
        message(WARNING
            "pytest not found for ${_turboq_python_executable} (pip install pytest) -- "
            "skipping registration of python/tests with ctest")
    endif()
endif()
