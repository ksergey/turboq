include(FetchContent)
FetchContent_Declare(doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
  EXCLUDE_FROM_ALL
  SYSTEM
)
FetchContent_MakeAvailable(doctest)
