include(FetchContent)
FetchContent_Declare(Boost
  URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
  EXCLUDE_FROM_ALL
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(Boost)

if (NOT TARGET Boost::scope_exit)
  if (NOT TARGET Boost::boost)
    message(FATAL_ERROR "target Boost::scope_exit not found")
  endif()
  add_library(boost_scope_exit INTERFACE)
  target_link_libraries(boost_scope_exit INTERFACE Boost::boost)
  add_library(Boost::scope_exit ALIAS boost_scope_exit)
endif()
