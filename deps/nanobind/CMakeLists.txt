include(FetchContent)

FetchContent_Declare(nanobind
  GIT_REPOSITORY https://github.com/wjakob/nanobind.git
  GIT_TAG v2.5.0
  GIT_SHALLOW OFF
)
FetchContent_MakeAvailable(nanobind)
