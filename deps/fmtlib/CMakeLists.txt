include(FetchContent)

FetchContent_Declare(fmtlib
  URL https://github.com/fmtlib/fmt/archive/refs/tags/11.0.1.tar.gz
  EXCLUDE_FROM_ALL
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(fmtlib)
