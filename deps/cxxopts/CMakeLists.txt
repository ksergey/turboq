include(FetchContent)

FetchContent_Declare(cxxopts
    URL https://github.com/jarro2783/cxxopts/archive/refs/tags/v3.3.1.tar.gz
    SYSTEM EXCLUDE_FROM_ALL OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(cxxopts)
