// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <x86intrin.h>

#include <algorithm>
#include <barrier>
#include <chrono>
#include <numeric>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include "platform.h"

namespace turboq::benchmark {

/// Do not optimize variable
template <typename T, typename D = std::decay_t<T>>
TURBOQ_FORCE_INLINE void doNotOptimize(T const& t) noexcept {
  // https://github.com/facebook/folly/blob/main/folly/lang/Hint-inl.h
  constexpr auto compilerMustForceIndirect =
      !std::is_trivially_copyable_v<D> or sizeof(long) < sizeof(D) or std::is_pointer_v<D>;

  if constexpr (compilerMustForceIndirect) {
    asm volatile("" : : "m"(t) : "memory");
  } else {
    asm volatile("" : : "r"(t));
  }
}

/// Return cycles counter
TURBOQ_FORCE_INLINE std::uint64_t rdtsc() noexcept {
  return __builtin_ia32_rdtsc();
}

TURBOQ_FORCE_INLINE void spinLoopPause() noexcept {
  _mm_pause();
}

/// Bind current thread to core
inline void bindCurrentThreadToCore(int coreNo) noexcept {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(coreNo, &cpuset);

  auto const rc = ::pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (rc != 0) {
    fmt::print(stderr, "failed to bind current thread to core: {}\n", ::strerror(rc));
  }
}

struct BenchmarkOptions {
  std::vector<int> producersCoreSet = {-1};
  std::vector<int> consumersCoreSet = {-1};
  std::size_t totalOps = 1000000;
  std::size_t repeats = 10;
};

template <typename ProduceFn, typename ConsumeFn, typename EndFn>
inline std::uint64_t runOnce(
    BenchmarkOptions const& opts, ProduceFn const& produceFn, ConsumeFn const& consumeFn, EndFn const& endFn) {
  int const producersCount = opts.producersCoreSet.size();
  int const consumersCount = opts.consumersCoreSet.size();

  std::barrier barrier(1 + producersCount + consumersCount);

  std::vector<std::thread> producersThr(producersCount);
  for (int tid = 0; tid < producersCount; ++tid) {
    producersThr[tid] = std::thread([&, coreNo = opts.producersCoreSet[tid], tid] {
      if (coreNo != -1) {
        bindCurrentThreadToCore(coreNo);
      }
      barrier.arrive_and_wait(); // A - wait for thread start
      barrier.arrive_and_wait(); // B - init the work
      produceFn(tid);
      barrier.arrive_and_wait(); // C - join the work
    });
  }

  std::vector<std::thread> consumersThr(consumersCount);
  for (int tid = 0; tid < consumersCount; ++tid) {
    consumersThr[tid] = std::thread([&, coreNo = opts.consumersCoreSet[tid], tid] {
      if (coreNo != -1) {
        bindCurrentThreadToCore(coreNo);
      }
      barrier.arrive_and_wait(); // A - wait for thread start
      barrier.arrive_and_wait(); // B - init the work
      consumeFn(tid);
      barrier.arrive_and_wait(); // C - join the work
    });
  }

  barrier.arrive_and_wait(); // A - wait for thread start

  auto const measureStart = std::chrono::steady_clock::now();

  barrier.arrive_and_wait(); // B - init the work
  barrier.arrive_and_wait(); // C - join the work

  endFn();

  for (auto& thread : producersThr) {
    thread.join();
  }
  for (auto& thread : consumersThr) {
    thread.join();
  }

  auto const measureEnd = std::chrono::steady_clock::now();

  return std::chrono::duration_cast<std::chrono::nanoseconds>(measureEnd - measureStart).count();
}

struct BenchmarkRunResult {
  std::uint64_t mean;
  std::uint64_t stddev;
};

template <typename RepeatFn>
inline BenchmarkRunResult runBench(BenchmarkOptions const& opts, RepeatFn const& repeatFn) {
  std::vector<std::uint64_t> durations(opts.repeats);

  repeatFn(); // heating

  for (std::size_t repeatNo = 0; repeatNo < opts.repeats; ++repeatNo) {
    std::uint64_t const duration = repeatFn();
    durations[repeatNo] = duration;
  }

  auto const mean = [&] {
    auto const sum = std::accumulate(durations.begin(), durations.end(), std::uint64_t(0));
    return std::uint64_t(sum * 1.0 / durations.size());
  }();

  auto const stddev = [&] {
    auto const sqSum = std::inner_product(durations.begin(), durations.end(), durations.begin(), std::uint64_t(0));
    return std::uint64_t(std::sqrt((sqSum / durations.size()) - (mean * mean)));
  }();

  return BenchmarkRunResult{mean / opts.totalOps, stddev / opts.totalOps};
}

inline void annotate(std::vector<std::tuple<char const*, BenchmarkRunResult>> const& results) {
  constexpr auto kNameFieldLength = 25;
  constexpr auto kValueFieldLength = 8;

  fmt::print(
      "{:<{}} {:>{}} {:>{}}\n", "name", kNameFieldLength, "mean", kValueFieldLength + 3, "stddev", kValueFieldLength + 3);
  fmt::print("{:-^{}}\n", "", 25 + 1 + 2 * (3 + kValueFieldLength + 1) - 1);

  for (auto const& [name, result] : results) {
    fmt::print("{:<{}} {:>{}} ns {:>{}} ns\n", name, kNameFieldLength, result.mean, kValueFieldLength, result.stddev,
        kValueFieldLength);
  }
}

} // namespace turboq::benchmark
