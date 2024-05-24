// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <vector>

#include <cxxopts.hpp>
#include <fmt/format.h>

#include "BoundedMPSCRawQueue.h"
#include "BoundedSPMCRawQueue.h"
#include "BoundedSPSCRawQueue.h"
#include "benchmark.h"
#include "concepts.h"
#include "testing.h"

using namespace turboq::benchmark;
using namespace turboq;

using turboq::testing::dequeue;
using turboq::testing::enqueue;

template <typename Queue>
BenchmarkRunResult benchmarkQueue(Queue& queue, BenchmarkOptions const& opts) {
  auto const producersCount = opts.producersCoreSet.size();
  auto const consumersCount = opts.consumersCoreSet.size();

  auto repeatFn = [&, ops = opts.totalOps] {
    std::atomic<std::uint64_t> sum(0);

    auto produceFn = [&](int tid) {
      auto producer = queue.createProducer();
      for (std::uint64_t i = tid; i < ops; i += producersCount) {
        while (!enqueue(producer, i)) {
          spinLoopPause();
        }
      }
    };
    auto consumeFn = [&](int tid) {
      auto consumer = queue.createConsumer();
      std::uint64_t consumerSum = 0;
      for (std::uint64_t i = tid; i < ops; i += consumersCount) {
        std::uint64_t value = 0;
        while (!dequeue(consumer, value)) {
          spinLoopPause();
        }
        assert(value == i);
        consumerSum += value;
      }
      sum.fetch_add(consumerSum);
    };
    auto endFn = [&] {
      std::uint64_t const expected = (ops) * (ops - 1) / 2;
      std::uint64_t const actual = sum.load();
      if (expected != actual) {
        fmt::print(stderr, "ERR: expected = {}, got = {}\n", expected, actual);
      }
    };

    return runOnce(opts, produceFn, consumeFn, endFn);
  };

  return runBench(opts, repeatFn);
}

void benchmarkBoundedMPSCRawQueue(std::vector<std::tuple<std::string, BenchmarkRunResult>>& results) {
  fmt::print(stdout, "benchmarkBoundedMPSCRawQueue ...\n");

  // init queue
  auto queue = BoundedMPSCRawQueue(
      "benchmark", BoundedMPSCRawQueue::CreationOptions(sizeof(int), 10000), AnonymousMemorySource());

  BenchmarkOptions opts;

  opts.producersCoreSet = {-1};
  results.emplace_back("MSPC p=1 c=1", benchmarkQueue(queue, opts));

  opts.producersCoreSet = {-1, -1};
  results.emplace_back("MSPC p=2 c=1", benchmarkQueue(queue, opts));

  opts.producersCoreSet = {-1, -1, -1};
  results.emplace_back("MSPC p=3 c=1", benchmarkQueue(queue, opts));

  opts.producersCoreSet = {-1, -1, -1, -1};
  results.emplace_back("MSPC p=4 c=1", benchmarkQueue(queue, opts));

  opts.consumersCoreSet = {0};
  opts.producersCoreSet = {1};
  results.emplace_back("MSPC/p p=1 c=1", benchmarkQueue(queue, opts));

  opts.producersCoreSet = {1, 2};
  results.emplace_back("MSPC/p p=2 c=1", benchmarkQueue(queue, opts));

  opts.producersCoreSet = {1, 2, 3};
  results.emplace_back("MSPC/p p=3 c=1", benchmarkQueue(queue, opts));

  opts.producersCoreSet = {1, 2, 3, 4};
  results.emplace_back("MSPC/p p=4 c=1", benchmarkQueue(queue, opts));
}

void benchmarkBoundedSPSCRawQueue(std::vector<std::tuple<std::string, BenchmarkRunResult>>& results) {
  fmt::print(stdout, "benchmarkBoundedSPSCRawQueue ...\n");

  // init queue
  auto queue =
      BoundedSPSCRawQueue("benchmark", BoundedSPSCRawQueue::CreationOptions(5 * (1 << 20)), AnonymousMemorySource());

  BenchmarkOptions opts;

  results.emplace_back("SPSC queue p=1 c=1", benchmarkQueue(queue, opts));

  opts.consumersCoreSet = {0};
  opts.producersCoreSet = {1};
  results.emplace_back("SPSC/p queue p=1 c=1", benchmarkQueue(queue, opts));
}

void benchmarkBoundedSPMCRawQueue(std::vector<std::tuple<std::string, BenchmarkRunResult>>& results) {
  // How to test?
  BenchmarkOptions opts;

  // init queue
  auto queue = BoundedSPMCRawQueue(
      "benchmark", BoundedSPMCRawQueue::CreationOptions(opts.totalOps * sizeof(int)), AnonymousMemorySource());

  opts.consumersCoreSet = {-1};
  results.emplace_back("SPMC queue p=1 c=1", benchmarkQueue(queue, opts));

  opts.consumersCoreSet = {-1, -1};
  results.emplace_back("SPMC queue p=1 c=2", benchmarkQueue(queue, opts));

  opts.consumersCoreSet = {-1, -1, -1};
  results.emplace_back("SPMC queue p=1 c=3", benchmarkQueue(queue, opts));

  opts.consumersCoreSet = {-1, -1, -1, -1};
  results.emplace_back("SPMC queue p=1 c=4", benchmarkQueue(queue, opts));
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
  try {
    cxxopts::Options options("turboq-benchmark2-bm", "TurboQ benchmark tool");
    // clang-format off
    options.add_options()
      ("only-mpsc", "benchmark only mpsc queue")
      ("only-spsc", "benchmark only spsc queue")
      ("help", "print help and exit")
    ;
    // clang-format on
    // TODO: add repeats, bind cpus, ops, etc options
    auto result = options.parse(argc, argv);

    if (result.count("help")) {
      fmt::print("{}\n", options.help());
      return EXIT_FAILURE;
    }

    std::vector<std::tuple<std::string, BenchmarkRunResult>> results;

    if (result.count("only-mpsc")) {
      benchmarkBoundedMPSCRawQueue(results);
    } else if (result.count("only-spsc")) {
      benchmarkBoundedSPSCRawQueue(results);
    } else {
      benchmarkBoundedMPSCRawQueue(results);
      benchmarkBoundedSPSCRawQueue(results);
      // benchmarkBoundedSPMCRawQueue(results);
    }

    fmt::print(stdout, "\n\n");

    annotate(results);

  } catch (std::exception const& e) {
    fmt::print(stderr, "ERROR: {}\n", e.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
