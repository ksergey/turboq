set(TargetName turboq)

add_library(${TargetName})
target_compile_features(${TargetName}
  PUBLIC cxx_std_20)
target_compile_options(${TargetName}
  PUBLIC -Wall -Wextra -Wattributes -Wpedantic -Wstrict-aliasing -Wcast-align -g)
target_compile_options(${TargetName}
  PRIVATE -fno-rtti)
target_include_directories(${TargetName}
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${TargetName}
  PUBLIC Boost::outcome Boost::scope_exit fmt::fmt)

file(GLOB_RECURSE Sources "${CMAKE_CURRENT_SOURCE_DIR}/turboq/*.cpp")
file(GLOB_RECURSE Headers "${CMAKE_CURRENT_SOURCE_DIR}/turboq/*.h")

TurboQAddTestsFromSourceList(Sources
  PREFIX ${TargetName}
  OPTIONS -Wall -Wextra
  LIBS doctest::doctest_with_main ${TargetName})

# TurboQAddBenchmarksFromSourceList(Sources
#   PREFIX ${TargetName}
#   OPTIONS -Wall -Wextra
#   LIBS benchmark::benchmark_main ${TargetName})

TurboQExcludeTestsAndBenchmarksFromSourceList(Sources)

foreach(file ${Headers})
  target_sources(${TargetName} PUBLIC ${file})
endforeach()

foreach(file ${Sources})
  target_sources(${TargetName} PRIVATE ${file})
endforeach()

add_library(${TargetName}::${TargetName} ALIAS ${TargetName})
