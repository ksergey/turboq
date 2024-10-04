find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)

set(TargetName pyturboq)

nanobind_add_module(${TargetName}
  NB_STATIC
  turboq.cpp)

target_link_libraries(${TargetName}
  PRIVATE turboq::turboq)

file(COPY dump.py DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
