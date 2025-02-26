[<img src="https://img.shields.io/github/license/ksergey/turboq">](https://opensource.org/license/agpl-v3)
[<img src="https://img.shields.io/github/actions/workflow/status/ksergey/turboq/build-and-test.yml?logo=linux">](https://github.com/ksergey/turboq/actions/workflows/build-and-test.yml)
[<img src="https://img.shields.io/badge/language-C%2B%2B20-red">](https://en.wikipedia.org/wiki/C%2B%2B23)

## turboq: message queues for low latency inter-process communications

> https://en.wikipedia.org/wiki/Message_queue

### Features

- Different queue types: MPSC, SPSC, SPMC
- Low latency

## Requirements

- C++20 (gcc-14+, clang-19+)
- benchmark, doctest, fmt

<!--
perf stat -e cache-references,cache-misses,L1-dcache-prefetches,instructions,cpu-cycles,branches,branch-misses,duration_time ./turboq-BoundedSPSCRawQueue-test
-->
