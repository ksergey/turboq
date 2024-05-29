include(CheckCXXSourceCompiles)

set(CMAKE_REQUIRED_FLAGS -fsanitize=address)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address)
check_cxx_source_compiles("int main() {}" Sanitizers_ADDRESS_FOUND)

if (Sanitizers_ADDRESS_FOUND)
  add_library(turboq_asan INTERFACE EXCLUDE_FROM_ALL)
  target_compile_options(turboq_asan INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(turboq_asan INTERFACE -fsanitize=address)
  add_library(turboq::asan ALIAS turboq_asan)
endif()

set(CMAKE_REQUIRED_FLAGS -fsanitize=thread -static-libtsan)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread -static-libtsan)
check_cxx_source_compiles("int main() {}" Sanitizers_THREAD_FOUND)

if (Sanitizers_THREAD_FOUND)
  add_library(turboq_tsan INTERFACE EXCLUDE_FROM_ALL)
  target_compile_options(turboq_tsan INTERFACE -fsanitize=thread -static-libtsan)
  target_link_options(turboq_tsan INTERFACE -fsanitize=thread -static-libtsan)
  add_library(turboq::tsan ALIAS turboq_tsan)
endif()

set(CMAKE_REQUIRED_FLAGS -fsanitize=undefined -static-libubsan)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=undefined -static-libubsan)
check_cxx_source_compiles("int main() {}" Sanitizers_UNDEFINED_FOUND)

if (Sanitizers_UNDEFINED_FOUND)
  add_library(turboq_ubsan INTERFACE EXCLUDE_FROM_ALL)
  target_compile_options(turboq_ubsan INTERFACE -fsanitize=undefined -static-libubsan)
  target_link_options(turboq_ubsan INTERFACE -fsanitize=undefined -static-libubsan)
  add_library(turboq::ubsan ALIAS turboq_ubsan)
endif()
