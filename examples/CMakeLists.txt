set(TargetName spsc_pub)

add_executable(${TargetName} ${TargetName}.cpp)
target_compile_features(${TargetName}
  PUBLIC cxx_std_23)
set_target_properties(${TargetName}
  PROPERTIES
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_compile_options(${TargetName}
  PUBLIC -Wall -Wextra -Wattributes -Wpedantic -Wstrict-aliasing -Wcast-align -g)
target_link_libraries(${TargetName}
  PUBLIC turboq::turboq)
