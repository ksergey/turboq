# turboq

[![C++](https://img.shields.io/badge/C++-23-blue.svg)](https://isocpp.org/)
[![License](https://img.shields.io/github/license/ksergey/turboq)](LICENSE)
[![CMake](https://img.shields.io/badge/build-CMake-informational.svg)](https://cmake.org)

**High-performance, low-latency message queue library in C++**

TurboQ is a lightweight C++ library for building low-latency message queues, designed for high-performance applications where every microsecond matters.

## Features

- **Ultra-low latency** - nanosecond-range queue operations
- **Lock-free algorithms** - maximum throughput with no locks
- **Multiple queue types** - SPSC, MPSC, MPMC queues
- **Zero-copy operations** - minimal memory overhead

## Quick Start

### Dependencies

- C++23 compiler
- CMake 3.24+

### Integration

Add to `CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(
    turboq
    GIT_REPOSITORY https://github.com/ksergey/turboq.git
    GIT_TAG main
)
FetchContent_MakeAvailable(turboq)

target_link_libraries(your_app PRIVATE turboq::turboq)

```

### Usage Examples

TODO

### License

Distributed under the AGPL-3.0 License. See LICENSE for details.

