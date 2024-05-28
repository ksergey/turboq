#include <atomic>
#include <barrier>
#include <cassert>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include <fmt/format.h>

#include "BoundedMPSCRawQueue.h"
#include "BoundedSPMCRawQueue.h"
#include "BoundedSPSCRawQueue.h"
#include "utils.h"

namespace turboq {

template <std::size_t SegmentSize>
struct Traits {
  static constexpr std::string_view kTag = "turboq/bm-only";
  static constexpr std::size_t kSegmentSize = SegmentSize;
  static constexpr std::size_t kAlign = kHardwareDestructiveInterferenceSize;
};

template <std::size_t SegmentSize>
struct SPMCQueue : BoundedSPMCRawQueueImpl<Traits<SegmentSize>> {
  SPMCQueue()
      : BoundedSPMCRawQueueImpl<Traits<SegmentSize>>("bm", {10 * std::size_t(1 << 20)}, AnonymousMemorySource()) {}
};

template <std::size_t SegmentSize>
struct SPSCQueue : BoundedSPSCRawQueueImpl<Traits<SegmentSize>> {
  SPSCQueue()
      : BoundedSPSCRawQueueImpl<Traits<SegmentSize>>("bm", {10 * std::size_t(1 << 20)}, AnonymousMemorySource()) {}
};

template <std::size_t SegmentSize>
struct MPSCQueue : BoundedMPSCRawQueueImpl<Traits<SegmentSize>> {
  MPSCQueue()
      : BoundedMPSCRawQueueImpl<Traits<SegmentSize>>(
            "bm", {std::size_t(sizeof(std::uint64_t)), 10 * std::size_t(1 << 10)}, AnonymousMemorySource()) {}
};

static void ApplyCustomArgs(::benchmark::internal::Benchmark* b) {
  b->MeasureProcessCPUTime();
  b->UseRealTime();
}

template <typename QueueT>
static void BM_EnqueueDequeue_NoThreads(::benchmark::State& state) {
  auto queue = QueueT();
  auto producer = queue.createProducer();
  auto consumer = queue.createConsumer();

  std::uint64_t counter = 0;
  std::uint64_t value = 0;

  for (auto _ : state) {
    while (!enqueue(producer, counter++)) {
    }
    while (!dequeue(consumer, value)) {
    }
    assert(value == (counter - 1));
    benchmark::DoNotOptimize(value);
  }

  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * sizeof(std::uint64_t));
}

BENCHMARK(BM_EnqueueDequeue_NoThreads<SPMCQueue<32>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue_NoThreads<SPMCQueue<64>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue_NoThreads<SPMCQueue<128>>)->Apply(ApplyCustomArgs);

BENCHMARK(BM_EnqueueDequeue_NoThreads<SPSCQueue<32>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue_NoThreads<SPSCQueue<64>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue_NoThreads<SPSCQueue<128>>)->Apply(ApplyCustomArgs);

BENCHMARK(BM_EnqueueDequeue_NoThreads<MPSCQueue<32>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue_NoThreads<MPSCQueue<64>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue_NoThreads<MPSCQueue<128>>)->Apply(ApplyCustomArgs);

template <typename QueueT>
static void BM_DequeueOnly_NoThreads(::benchmark::State& state) {
  auto queue = QueueT();
  auto consumer = queue.createConsumer();

  std::uint64_t value = 0;

  for (auto _ : state) {
    dequeue(consumer, value);
    assert(value == 0);
    benchmark::DoNotOptimize(value);
  }
}

BENCHMARK(BM_DequeueOnly_NoThreads<SPMCQueue<32>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_DequeueOnly_NoThreads<SPMCQueue<64>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_DequeueOnly_NoThreads<SPMCQueue<128>>)->Apply(ApplyCustomArgs);

BENCHMARK(BM_DequeueOnly_NoThreads<SPSCQueue<32>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_DequeueOnly_NoThreads<SPSCQueue<64>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_DequeueOnly_NoThreads<SPSCQueue<128>>)->Apply(ApplyCustomArgs);

BENCHMARK(BM_DequeueOnly_NoThreads<MPSCQueue<32>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_DequeueOnly_NoThreads<MPSCQueue<64>>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_DequeueOnly_NoThreads<MPSCQueue<128>>)->Apply(ApplyCustomArgs);

struct BindToCore {};

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

template <std::size_t ProducersCount, std::size_t ConsumersCount, typename BindToCoreT, typename ProduceFn,
    typename ConsumeFn, typename EndFn>
inline std::uint64_t runOnce(ProduceFn const& produceFn, ConsumeFn const& consumeFn, EndFn const& endFn) {
  std::barrier barrier(1 + ProducersCount + ConsumersCount);

  std::vector<std::thread> producersThr(ProducersCount);
  for (int tid = 0; tid < int(ProducersCount); ++tid) {
    producersThr[tid] = std::thread([&, tid] {
      if constexpr (std::is_same_v<BindToCoreT, BindToCore>) {
        bindCurrentThreadToCore(tid);
      }

      barrier.arrive_and_wait(); // A - wait for thread start
      barrier.arrive_and_wait(); // B - init the work

      produceFn(tid);

      barrier.arrive_and_wait(); // C - join the work
    });
  }

  std::vector<std::thread> consumersThr(ConsumersCount);
  for (int tid = 0; tid < int(ConsumersCount); ++tid) {
    consumersThr[tid] = std::thread([&, tid] {
      if constexpr (std::is_same_v<BindToCoreT, BindToCore>) {
        bindCurrentThreadToCore(tid + ProducersCount);
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

template <typename QueueT, std::size_t ProducersCount, std::size_t ConsumersCount, std::size_t Ops,
    typename BindToCoreT = void>
static void BM_EnqueueDequeue(::benchmark::State& state) {
  static_assert(ProducersCount > 0 and ConsumersCount > 0 and Ops > 0);
  static_assert(ProducersCount == 1 or ConsumersCount == 1);

  auto const repeatFn = [&] {
    auto queue = QueueT();
    auto sum = std::atomic<std::uint64_t>(0);

    auto const produceFn = [&](int tid) {
      auto producer = queue.createProducer();
      for (std::uint64_t i = tid; i < Ops; i += ProducersCount) {
        while (!enqueue(producer, i)) {
          ::benchmark::DoNotOptimize(i);
        }
      }
    };
    auto const consumeFn = [&](int tid) {
      auto consumer = queue.createConsumer();
      std::uint64_t consumerSum = 0;
      for (std::uint64_t i = tid; i < Ops; i += ConsumersCount) {
        std::uint64_t value = 0;
        while (!dequeue(consumer, value)) {
          ::benchmark::DoNotOptimize(i);
        }
        consumerSum += value;
      }
      sum.fetch_add(consumerSum);
    };
    auto endFn = [&] {
      std::uint64_t const expected = (Ops) * (Ops - 1) / 2;
      std::uint64_t const actual = sum.load();
      if (expected != actual) {
        state.SkipWithError(fmt::format("Expected sum {}, got {}", expected, actual));
      }
    };
    return runOnce<ProducersCount, ConsumersCount, BindToCoreT>(produceFn, consumeFn, endFn);
  };

  std::vector<std::uint64_t> durations;

  for (auto _ : state) {
    auto const ns = repeatFn();
    state.PauseTiming();
    durations.push_back(ns);
    state.ResumeTiming();
  }

  state.SetItemsProcessed(state.iterations() * Ops);
  state.SetBytesProcessed(state.iterations() * Ops * sizeof(std::uint64_t));

  auto const mean = [&] {
    auto const sum = std::accumulate(durations.begin(), durations.end(), std::uint64_t(0));
    return std::uint64_t(sum * 1.0 / durations.size());
  }();

  auto const stddev = [&] {
    auto const sqSum = std::inner_product(durations.begin(), durations.end(), durations.begin(), std::uint64_t(0));
    return std::uint64_t(std::sqrt((sqSum / durations.size()) - (mean * mean)));
  }();

  state.counters["mean"] = ::benchmark::Counter(double(mean) / Ops);
  state.counters["stddev"] = ::benchmark::Counter(double(stddev) / Ops);
}

static constexpr std::size_t kOps = 1000000;

BENCHMARK(BM_EnqueueDequeue<SPSCQueue<32>, 1, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<SPSCQueue<32>, 1, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<SPSCQueue<64>, 1, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<SPSCQueue<64>, 1, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<SPSCQueue<128>, 1, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<SPSCQueue<128>, 1, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);

BENCHMARK(BM_EnqueueDequeue<MPSCQueue<32>, 1, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<32>, 1, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<64>, 1, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<64>, 1, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<128>, 1, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<128>, 1, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<32>, 2, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<32>, 2, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<64>, 2, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<64>, 2, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<128>, 2, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<128>, 2, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<32>, 4, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<32>, 4, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<64>, 4, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<64>, 4, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<128>, 4, 1, kOps>)->Apply(ApplyCustomArgs);
BENCHMARK(BM_EnqueueDequeue<MPSCQueue<128>, 4, 1, kOps, BindToCore>)->Apply(ApplyCustomArgs);

} // namespace turboq
