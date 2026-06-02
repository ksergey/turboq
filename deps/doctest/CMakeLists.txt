include(FetchContent)

FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/releases/download/v2.5.2/doctest-v2.5.2.tar.gz
    SYSTEM EXCLUDE_FROM_ALL OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(doctest)
