// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <vector>

#include <fmt/format.h>

#include "BoundedMPSCRawQueue.h"
#include "BoundedSPMCRawQueue.h"
#include "BoundedSPSCRawQueue.h"
#include "benchmark.h"
#include "concepts.h"

using namespace turboq::benchmark;
using namespace turboq;

template <typename P>
  requires turboq::TurboQProducer<P>
void enqueue(P& producer, std::uint64_t value) {
  do {
    auto buffer = producer.prepare(sizeof(value));
    if (buffer.size() == 0) {
      spinLoopPause();
      continue;
    }
    *std::bit_cast<std::uint64_t*>(buffer.data()) = value;
    producer.commit();
  } while (false);
}

template <typename C>
  requires turboq::TurboQConsumer<C>
void dequeue(C& consumer, std::uint64_t& value) {
  do {
    auto buffer = consumer.fetch();
    if (buffer.size() == 0) {
      spinLoopPause();
      continue;
    }
    value = *std::bit_cast<std::uint64_t const*>(buffer.data());
  } while (false);
}

template <typename Queue>
BenchmarkRunResult benchmarkQueue(Queue& queue, BenchmarkOptions const& opts) {
  auto const producersCount = opts.producersCoreSet.size();
  auto const consumersCount = opts.consumersCoreSet.size();

  auto repeatFn = [&, ops = opts.totalOps] {
    auto produceFn = [&](int tid) {
      auto producer = queue.createProducer();
      for (std::uint64_t i = tid; i < ops; i += producersCount) {
        enqueue(producer, i);
      }
    };
    auto consumeFn = [&](int tid) {
      auto consumer = queue.createConsumer();
      for (std::uint64_t i = tid; i < ops; i += consumersCount) {
        std::uint64_t value = 0;
        dequeue(consumer, value);
        doNotOptimize(value);
      }
    };
    auto endFn = [&] {};

    return runOnce(opts, produceFn, consumeFn, endFn);
  };

  return runBench(opts, repeatFn);
}

void benchmarkBoundedMPSCRawQueue(std::vector<std::tuple<char const*, BenchmarkRunResult>>& results) {
  // init queue
  auto queue = BoundedMPSCRawQueue(
      "benchmark", BoundedMPSCRawQueue::CreationOptions(sizeof(int), 10000), AnonymousMemorySource());

  BenchmarkOptions opts;

  opts.producersCoreSet = {-1};
  results.emplace_back("mpsc queue p=1 c=1", benchmarkQueue(queue, opts));

  opts.producersCoreSet = {-1, -1};
  results.emplace_back("mpsc queue p=2 c=1", benchmarkQueue(queue, opts));

  opts.producersCoreSet = {-1, -1, -1};
  results.emplace_back("mpsc queue p=3 c=1", benchmarkQueue(queue, opts));

  opts.producersCoreSet = {-1, -1, -1, -1};
  results.emplace_back("mpsc queue p=4 c=1", benchmarkQueue(queue, opts));
}

void benchmarkBoundedSPSCRawQueue(std::vector<std::tuple<char const*, BenchmarkRunResult>>& results) {
  // init queue
  auto queue = BoundedSPSCRawQueue(
      "benchmark", BoundedSPSCRawQueue::CreationOptions(1000 * sizeof(int)), AnonymousMemorySource());

  BenchmarkOptions opts;

  results.emplace_back("spsc queue p=1 c=1", benchmarkQueue(queue, opts));
}

void benchmarkBoundedSPMCRawQueue(std::vector<std::tuple<char const*, BenchmarkRunResult>>& results) {
  BenchmarkOptions opts;

  // init queue
  auto queue = BoundedSPMCRawQueue(
      "benchmark", BoundedSPMCRawQueue::CreationOptions(opts.totalOps * sizeof(int)), AnonymousMemorySource());

  opts.consumersCoreSet = {-1};
  results.emplace_back("spmc queue p=1 c=1", benchmarkQueue(queue, opts));

  opts.consumersCoreSet = {-1, -1};
  results.emplace_back("spmc queue p=1 c=2", benchmarkQueue(queue, opts));

  opts.consumersCoreSet = {-1, -1, -1};
  results.emplace_back("spmc queue p=1 c=3", benchmarkQueue(queue, opts));

  opts.consumersCoreSet = {-1, -1, -1, -1};
  results.emplace_back("spmc queue p=1 c=4", benchmarkQueue(queue, opts));
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
  try {
    std::vector<std::tuple<char const*, BenchmarkRunResult>> results;

    benchmarkBoundedMPSCRawQueue(results);
    benchmarkBoundedSPSCRawQueue(results);
    benchmarkBoundedSPMCRawQueue(results);

    annotate(results);

  } catch (std::exception const& e) {
    fmt::print(stderr, "ERROR: {}\n", e.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
