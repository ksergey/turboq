// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <emmintrin.h>
#include <pthread.h>
#include <x86intrin.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <thread>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include "BoundedMPSCRawQueue.h"
#include "BoundedSPMCRawQueue.h"
#include "BoundedSPSCRawQueue.h"
#include "concepts.h"
#include "platform.h"

namespace bm {

/// Get current value of cycles counter
TURBOQ_FORCE_INLINE auto rdtsc() noexcept {
  return __builtin_ia32_rdtsc();
}

using cycles_t = decltype(rdtsc());

template <std::size_t S>
struct Data {
  std::uint8_t data[S];

  Data& fill(std::uint8_t value) noexcept {
    for (std::uint8_t& ref : data) {
      ref = value;
    }
    return *this;
  }
};

struct Stats {
  cycles_t min = 0;
  cycles_t max = 0;
  cycles_t q50 = 0;
  cycles_t q90 = 0;
  cycles_t q99 = 0;
  cycles_t q999 = 0;
};

Stats calculateStats(std::vector<cycles_t>& storage) noexcept {
  if (storage.empty()) {
    return Stats();
  }

  std::sort(storage.begin(), storage.end());

  Stats stats;
  stats.min = storage.front();
  stats.max = storage.back();
  stats.q50 = storage[storage.size() * 0.5];
  stats.q90 = storage[storage.size() * 0.9];
  stats.q99 = storage[storage.size() * 0.99];
  stats.q999 = storage[storage.size() * 0.999];

  return stats;
}

TURBOQ_FORCE_INLINE void spinLoopPause() noexcept {
  _mm_pause();
}

void pinThreadToCore(std::thread& t, int coreNo) noexcept {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(coreNo, &cpuset);

  auto const rc = ::pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
  if (rc != 0) {
    fmt::print(stderr, "failed to pin thread to core: {}\n", ::strerror(rc));
  }
}

template <typename T>
TURBOQ_FORCE_INLINE void doNotOptimize(T& value) noexcept {
  asm volatile("" : "+r,m"(value) : : "memory");
}

template <typename D, typename P, typename Fn>
  requires turboq::Producer<P> && std::is_trivially_copyable_v<D>
void push(P& producer, Fn&& fn) {
  do {
    auto buffer = producer.prepare(sizeof(D));
    if (buffer.size() == 0) {
      spinLoopPause();
      continue;
    }
    std::invoke(std::forward<Fn>(fn), std::bit_cast<D*>(buffer.data()));
    producer.commit();
  } while (false);
}

template <typename D, typename C, typename Fn>
  requires turboq::Consumer<C> && std::is_trivially_copyable_v<D>
bool pop(C& consumer, Fn&& fn) {
  auto buffer = consumer.fetch();
  if (buffer.size() == 0) {
    return false;
  }
  std::invoke(std::forward<Fn>(fn), std::bit_cast<D const*>(buffer.data()));
  return true;
}

template <typename D, typename P>
void producerTask(P& producer, std::atomic<int>& latch, std::size_t iterations, cycles_t* storage) {
  while (latch.load() == 0) {
  }

  for (std::size_t i = 0; i < iterations; ++i) {
    auto t0 = rdtsc();
    push<D>(producer, [i](D* data) {
      data->fill(i);
    });
    auto t1 = rdtsc();

    storage[i] = (t1 - t0);
  }
}

template <typename D, typename C>
void consumerTask(C& consumer, std::atomic<int>& latch, std::size_t iterations, cycles_t* storage) {
  while (latch.load() == 0) {
  }

  for (std::size_t i = 0; i < iterations; ++i) {
    auto t0 = rdtsc();
    pop<D>(consumer, [](D const* data) {
      doNotOptimize(data);
    });
    auto t1 = rdtsc();

    storage[i] = (t1 - t0);
  }
}

template <typename D, typename QueueT>
void run(QueueT queue, std::size_t iterations, int producerCoreNo = -1, int consumerCoreNo = -1) {

  std::atomic<int> latch = 0;

  Stats producerStats;
  Stats consumerStats;

  // start producer thread
  auto producerThread = std::thread(
      [&](QueueT::Producer producer) {
        std::vector<cycles_t> storage(iterations);
        producerTask<D>(producer, latch, iterations, storage.data());
        producerStats = calculateStats(storage);
      },
      queue.createProducer());
  if (producerCoreNo != -1) {
    pinThreadToCore(producerThread, producerCoreNo);
  }

  // start consumer thread
  auto consumerThread = std::thread(
      [&](QueueT::Consumer consumer) {
        std::vector<cycles_t> storage(iterations);
        consumerTask<D>(consumer, latch, iterations, storage.data());
        consumerStats = calculateStats(storage);
      },
      queue.createConsumer());
  if (consumerCoreNo != -1) {
    pinThreadToCore(consumerThread, consumerCoreNo);
  }

  // start
  latch.store(1);

  producerThread.join();
  consumerThread.join();

  auto showStats = [](FILE* fp, char const* description, Stats const& stats) {
    fmt::print(fp, "{:>10} [min/max/q50/q90/q99/q99.9] = {}/{}/{}/{}/{}/{}\n", description, stats.min, stats.max,
        stats.q50, stats.q90, stats.q99, stats.q999);
  };

  fmt::print(stdout, "iterations = {}, size = {}, producer-core = {}, consumer-core = {}\n", iterations, sizeof(D),
      producerCoreNo, consumerCoreNo);
  showStats(stdout, "producer", producerStats);
  showStats(stdout, "consumer", producerStats);
}

template <typename D>
void benchmarkSPSC(std::size_t iterations, int producerCoreNo = -1, int consumerCoreNo = -1,
    turboq::MemorySource const& memorySource = turboq::DefaultMemorySource()) {
  try {
    run<D>(turboq::BoundedSPSCRawQueue("bm-spsc", turboq::BoundedSPSCRawQueue::CreationOptions(100000), memorySource),
        iterations, producerCoreNo, consumerCoreNo);
  } catch (std::exception const& e) {
    fmt::print(stderr, "ERROR: {}\n", e.what());
  }
}

template <typename D>
void benchmarkMPSC(std::size_t iterations, int producerCoreNo = -1, int consumerCoreNo = -1,
    turboq::MemorySource const& memorySource = turboq::DefaultMemorySource()) {
  try {
    run<D>(turboq::BoundedMPSCRawQueue(
               "bm-mpsc", turboq::BoundedMPSCRawQueue::CreationOptions(sizeof(D), 1000), memorySource),
        iterations, producerCoreNo, consumerCoreNo);
  } catch (std::exception const& e) {
    fmt::print(stderr, "ERROR: {}\n", e.what());
  }
}

} // namespace bm

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
  using namespace turboq;

  fmt::print("SPSC queue, default memory source (huge pages = none)\n");
  bm::benchmarkSPSC<bm::Data<1>>(1000000, -1, -1, DefaultMemorySource());

  fmt::print("SPSC queue, default memory source (huge pages = none)\n");
  bm::benchmarkSPSC<bm::Data<1>>(1000000, 1, 5, DefaultMemorySource());

  fmt::print("SPSC queue, default memory source (huge pages = 2M)\n");
  bm::benchmarkSPSC<bm::Data<1>>(1000000, -1, -1, DefaultMemorySource(HugePagesOption::HugePages2M));

  fmt::print("SPSC queue, default memory source (huge pages = 2M)\n");
  bm::benchmarkSPSC<bm::Data<1>>(1000000, 1, 5, DefaultMemorySource(HugePagesOption::HugePages2M));

  fmt::print("MPSC queue, default memory source (huge pages = none)\n");
  bm::benchmarkMPSC<bm::Data<1>>(1000000, -1, -1, DefaultMemorySource());

  fmt::print("MPSC queue, default memory source (huge pages = none)\n");
  bm::benchmarkMPSC<bm::Data<1>>(1000000, 1, 5, DefaultMemorySource());

  fmt::print("MPSC queue, default memory source (huge pages = 2M)\n");
  bm::benchmarkMPSC<bm::Data<1>>(1000000, -1, -1, DefaultMemorySource(HugePagesOption::HugePages2M));

  fmt::print("MPSC queue, default memory source (huge pages = 2M)\n");
  bm::benchmarkMPSC<bm::Data<1>>(1000000, 1, 5, DefaultMemorySource(HugePagesOption::HugePages2M));

  return EXIT_SUCCESS;
}
