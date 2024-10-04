set(TargetName turboq)

add_library(${TargetName})
target_compile_features(${TargetName}
  PUBLIC cxx_std_20)
set_target_properties(${TargetName}
  PROPERTIES
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_compile_options(${TargetName}
  PUBLIC -Wall -Wextra -Wattributes -Wpedantic -Wstrict-aliasing -Wcast-align -g)
target_include_directories(${TargetName}
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${TargetName}
  PUBLIC fmt::fmt-header-only Boost::outcome Boost::scope_exit)

if (TURBOQ_PYTHON)
  set_property(TARGET ${TargetName}
    PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

if (TURBOQ_SANITIZER)
  target_compile_options(${TargetName}
    PUBLIC -fsanitize=address,leak,undefined)
  target_link_options(${TargetName}
    PUBLIC -fsanitize=address,leak,undefined)
endif()

file(GLOB_RECURSE Sources "${CMAKE_CURRENT_SOURCE_DIR}/turboq/*.cpp")
file(GLOB_RECURSE Headers "${CMAKE_CURRENT_SOURCE_DIR}/turboq/*.h")

TurboQAddTestsFromSourceList(Sources
  PREFIX ${TargetName}
  COMPILE_OPTIONS -Wall -Wextra -g
  LIBS doctest::doctest_with_main ${TargetName})

TurboQAddBenchmarksFromSourceList(Sources
  PREFIX ${TargetName}
  COMPILE_OPTIONS -Wall -Wextra -g
  LIBS benchmark::benchmark_main ${TargetName})

TurboQExcludeTestsAndBenchmarksFromSourceList(Sources)

foreach(file ${Headers})
  target_sources(${TargetName} PUBLIC ${file})
endforeach()

foreach(file ${Sources})
  target_sources(${TargetName} PRIVATE ${file})
endforeach()

add_library(${TargetName}::${TargetName} ALIAS ${TargetName})
