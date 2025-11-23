include(FetchContent)
FetchContent_Declare(cxxopts
  URL https://github.com/jarro2783/cxxopts/archive/refs/tags/v3.2.0.tar.gz
  EXCLUDE_FROM_ALL
  SYSTEM
)
FetchContent_MakeAvailable(cxxopts)
