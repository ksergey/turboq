include(FetchContent)
FetchContent_Declare(Boost
  URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
  EXCLUDE_FROM_ALL
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(Boost)

if (NOT TARGET Boost::outcome)
  if (NOT TARGET Boost::boost)
    message(FATAL_ERROR "target Boost::outcome not found")
  endif()
  add_library(boost_outcome INTERFACE)
  target_link_libraries(boost_outcome INTERFACE Boost::boost)
  add_library(Boost::outcome ALIAS boost_outcome)
endif()
