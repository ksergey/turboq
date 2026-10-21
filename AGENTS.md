# AGENTS.md

Guidance for AI coding agents working on **turboq** — a C++23, Linux-only library of lock-free,
zero-copy, shared-memory message queues (SPSC, MPSC, Multicast) with optional pybind11 bindings.
Latency is the product: every change to a queue's hot path must be justified in those terms.

## Repository map

| Path | What lives there |
|---|---|
| `code/turboq/*.h` | Queue implementations (header-only templates): `SPSCMessageQueue.h`, `MPSCMessageQueue.h`, `MulticastMessageQueue.h` |
| `code/turboq/*.cpp` | Non-template support: `File`, `MappedRegion`, `MemorySource` (shm / hugepages / memfd) |
| `code/turboq/*_test.cpp` | doctest unit tests, one executable per file (auto-discovered) |
| `code/turboq/TestUtils.h` | `enqueue`/`dequeue`/`fetch` helpers and `MemorySourceFixture` |
| `code/turboq/Concepts.h` | `Producer` / `Consumer` concepts every queue side must satisfy |
| `code/turboq/Error.h` | `turboq::Error` enum + error categories |
| `tools/latency_bench.cpp` | Inter-process latency benchmark (producer or consumer per run); `tools/Common.h` helpers |
| `python/_turboq.cpp` | pybind11 module `_turboq`; `python/turboq/__init__.py` re-exports it |
| `python/tests/` | pytest suite for the bindings |
| `cmake/` | `GetCPM.cmake` (dependency bootstrap), `TurboQHelpers.cmake` (test/bench discovery) |
| `.github/workflows/build-and-test.yml` | CI: compiler matrix, sanitizers, Python job |

## Toolchain

- **GCC 14+ or Clang 20+** is required (`<print>`, `<expected>`, C++23). GCC 13 fails on
  `#include <print>`; always pass the compiler explicitly if the system default is older.
- CMake 3.24+, Ninja recommended.
- The first configure needs network access: CPM fetches doctest, cxxopts and (with
  `turboq_PYTHON=ON`) pybind11. Set `CPM_SOURCE_CACHE` to reuse downloads across build dirs.

## Build and test

```bash
# Debug build + C++ tests (what CI runs per compiler)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14
cmake --build build
ctest --test-dir build/code -j"$(nproc)" --output-on-failure

# Run one suite / one case directly
./build/code/turboq-SPSCMessageQueue-test
./build/code/turboq-SPSCMessageQueue-test -tc="basic"

# Sanitizers -- two separate builds, they cannot be combined (CMake errors out)
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14 -Dturboq_SANITIZER=ON
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14 -Dturboq_TSAN=ON
# then build each and run ctest with:
#   ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
#   TSAN_OPTIONS=halt_on_error=1

# Python bindings (needs python3-dev; pytest must be importable at configure time,
# otherwise the pytest suite is silently not registered with ctest)
cmake -B build-py -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -Dturboq_PYTHON=ON
cmake --build build-py --target _turboq
ctest --test-dir build-py/python --output-on-failure
```

CMake options: `turboq_BUILD_TESTS` (ON), `turboq_TOOLS` (ON), `turboq_PYTHON` (OFF),
`turboq_SANITIZER` (OFF, ASan+UBSan+LSan), `turboq_TSAN` (OFF).

Before declaring a change done: the Debug build must be warning-free, all ctest suites must pass,
and anything touching atomics, memory ordering or the shared-memory layout must also pass the TSan
and ASan builds. If you touched `python/`, run the Python suite too.

## Adding tests

- Name the file `code/turboq/<Name>_test.cpp`; `TurboQAddTestsFromSourceList` turns it into
  `turboq-<Name>-test` and registers it with ctest automatically (sources are globbed —
  re-run the CMake configure after adding a file). No CMake edits needed.
- Wrap cases in `namespace turboq::testing { TEST_SUITE("...") { ... } }`, as existing tests do.
- Use `AnonymousMemorySource{}` for single-handle tests. It creates a fresh memfd on **every**
  `open()` regardless of name, so tests that open the same queue twice (tag/size mismatch,
  single-producer enforcement, open-only) must use `MemorySourceFixture::makeTempMemorySource()`.
- Custom `Options` structs need namespace scope (local classes can't have static members).
- Files named `*_bm.cpp` are picked up as benchmarks linked to `benchmark::benchmark_main`, but
  google-benchmark is not fetched by CMake today — don't add one without also adding the dependency.

## Code style

- Format with the repo's `.clang-format` (4-space indent, 120 columns, `PointerAlignment: Left`).
  Format only the lines you change; clang-format versions disagree on parts of the existing code,
  so don't reformat whole files in an unrelated change.
- Every source file starts with:
  ```cpp
  // Copyright (c) Sergey Kovalevich <inndie@gmail.com>
  // SPDX-License-Identifier: MIT
  ```
  followed by `#pragma once` for headers. Includes: standard headers first, then local `"..."` ones.
- Trailing return types (`auto f() -> T`), `[[nodiscard]]` on anything returning a value,
  `noexcept` where it holds. Doxygen-style `///` comments on public API.
- Naming: `PascalCase` types, `camelCase` functions/variables, `member_` with trailing underscore,
  `kConstant` for constants, `TURBOQ_` prefix for macros. Implementation templates live in
  `turboq::detail` (`...Impl<Options>`); public names are aliases over a `...OptionsDefault`.
- Hot-path members use `TURBOQ_FORCE_INLINE`; cold paths can use `TURBOQ_COLD`.
- Don't add free functions named `toString` in `namespace turboq`: doctest stringifies values via
  unqualified `toString(...)`, and ADL picks ours up and breaks every `REQUIRE_EQ` on turboq types.
- The library is built with `-fno-rtti` (propagated as a PUBLIC flag): no `dynamic_cast`/`typeid`.
  Only the Python module re-enables RTTI (via `_turboq_rtti_override`).
- Shared state is accessed through `std::atomic_ref` on plain fields of trivially-copyable headers;
  keep the `static_assert`s on `is_always_lock_free` and `is_trivially_copyable_v`. Fields written
  by different sides go on separate cache lines (`alignas(kCacheLineSize)`).

## Error handling convention

- Constructors that open/create a queue throw `std::system_error` with a `turboq::Error` or POSIX
  error code.
- `makeQueue` / `makeProducer` / `makeConsumer` are the `noexcept` wrappers returning
  `std::expected<T, std::error_code>`. Keep both forms in sync when adding constructor overloads.
- New failure modes get a new `turboq::Error` enumerator **and** a message in
  `ErrorCategory::message()`.
- Hot-path operations (`prepare`, `commit`, `fetch`, `consume`) never throw or block; they signal
  "full" / "empty" with an empty span. MPSC `prepare()` is the exception: it throws if the message
  exceeds the slot size.

## Invariants you must not break

- **On-disk layout is a compatibility contract.** Queue files are shared between independently
  built processes. Any change to a `MemoryHeader`, message/slot header, alignment or the offset
  math is a breaking change: call it out explicitly, and change the default `tag` if old and new
  layouts could otherwise be opened by each other.
- On open, the tag, capacity and `reservedSize` are validated (`TagMismatch`, `SizeMismatch`).
  Preserve these checks, and keep the open-only path rejecting files smaller than the header
  (`BufferTooSmall`) — mapping a too-small file causes SIGBUS, not a catchable error.
- Every `Options` struct must declare `tag` and `reserveSpace`. With `reserveSpace == 0` the layout
  must stay byte-identical to the pre-feature layout.
- SPSC and Multicast are single-producer, enforced with OFD file-region locks plus the
  `producerCreated_`/`consumerCreated_` flags (same-fd re-locking always succeeds); keep both.
- Multicast has no backpressure by design: slow consumers can be overrun; lap detection is
  best-effort. Don't "fix" this by making the producer wait.
- MPSC is a fixed-size array of power-of-two slots with separate per-slot headers on their own
  cache lines; reservation (CAS on `producerPos`) and publication (`committed` flag) are distinct
  steps — that separation is what makes concurrent producers safe.
- `commit(size)` requires `size <=` the size passed to `prepare()`.
- Never add locks, allocations, syscalls or logging to `prepare`/`commit`/`fetch`/`consume`.

## Python bindings

- API is bytes-in/bytes-out: `producer.send(bytes) -> bool`, `consumer.receive() -> bytes | None`.
  A zero-length message is indistinguishable from "empty" and returns `None` (by design).
- C++ errors surface as `RuntimeError`.
- When you change C++ queue APIs, update `python/_turboq.cpp`, the docstrings, `python/tests/`, and
  the Python section of `README.md` together.
- `python/pyproject.toml` builds from the repo root via scikit-build-core with tests off and only
  the `_turboq` target; keep it that way.

## Benchmarks

`tools/latency_bench` measures real inter-process latency (start the producer first; producer
and consumer must use the same `--hugepages` value). Numbers from an unpinned or virtualized
machine are noise — don't cite them as evidence of a performance improvement. Real measurements
need isolated, pinned cores (`taskset`) and huge pages; see "Performance tuning" in `README.md`.

## Docs and commits

- `README.md` is the user documentation; update it when public API, CMake options, or behaviour
  described there changes.
- Keep commits focused, with short imperative messages (`python: ...` prefix for bindings-only
  changes). Don't commit `build*/` directories or Python caches.
