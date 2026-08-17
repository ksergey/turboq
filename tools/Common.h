// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT
//
// Shared between rate_producer / rate_consumer: wire format, monotonic clock
// helper and a SIGINT/SIGTERM latch so both tools can be interrupted cleanly.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

#include <turboq/Platform.h>

namespace bench {

/// Wire format written into each queue slot. If the requested message size is
/// larger than sizeof(Message), the remaining bytes are left as-is (padding).
struct Message {
    std::uint64_t seq;       // 1-based, strictly monotonically increasing
    std::int64_t sendTimeNs; // steady_clock timestamp, captured right before commit()
};

/// Monotonic clock. Producer and consumer must run on the same host for the
/// resulting latency values to be meaningful (no clock sync across hosts).
[[nodiscard]] inline auto clockNow() noexcept -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Which clock the per-message send timestamp is stamped with -- see readTsc()/TscCalibration
/// below for why the tsc option needs extra care on the reading side.
enum class ClockSource { Wall, Tsc };

#if defined(__x86_64__) || defined(__i386__)
inline constexpr bool kTscSupported = true;
#else
inline constexpr bool kTscSupported = false;
#endif

/// Raw TSC tick count via the plain (non-serializing) RDTSC instruction. Cheaper than
/// clock_gettime() (no vDSO/syscall, just a handful of cycles), which is the whole point of
/// offering it -- but the ticks it returns are NOT nanoseconds and are only meaningful compared
/// against another tick count from the SAME machine, converted through TscCalibration below.
///
/// Two caveats worth knowing before trusting numbers produced with this:
///  - RDTSC isn't serializing: the CPU can execute it out of order relative to surrounding
///    instructions, which adds a few cycles of jitter to any single reading. Irrelevant at the
///    message-latency scale this tool measures (hundreds of ns and up), but don't reuse this for
///    timing something a handful of instructions long without adding your own fencing.
///  - This assumes an invariant TSC that's synchronized across cores/sockets (true on essentially
///    all x86_64 hardware from the last ~15 years, and required reading if you're pinning
///    producer/consumer to different cores/sockets via taskset per the README's tuning section --
///    check for the constant_tsc and nonstop_tsc flags in /proc/cpuinfo if in doubt).
[[nodiscard]] inline auto readTsc() noexcept -> std::uint64_t {
    if constexpr (kTscSupported) {
        return __builtin_ia32_rdtsc();
    } else {
        return 0;
    }
}

/// One-time, per-process calibration of the TSC's tick rate against the (trusted) steady_clock,
/// needed to convert a raw readTsc() delta into nanoseconds. Takes ~50ms; construct once at
/// startup, not on the hot path.
class TscCalibration {
private:
    double nsPerTick_{1.0};

public:
    TscCalibration() noexcept {
        constexpr auto kCalibrationWindow = std::chrono::milliseconds(50);

        auto const startNs = clockNow();
        auto const startTicks = readTsc();
        std::this_thread::sleep_for(kCalibrationWindow);
        auto const endNs = clockNow();
        auto const endTicks = readTsc();

        auto const elapsedNs = static_cast<double>(endNs - startNs);
        auto const elapsedTicks = static_cast<double>(endTicks - startTicks);
        nsPerTick_ = elapsedTicks > 0.0 ? elapsedNs / elapsedTicks : 1.0;
    }

    /// Converts a *delta* between two readTsc() calls into nanoseconds. Not meaningful applied to
    /// a single raw reading -- readTsc() has no defined epoch, only differences mean anything.
    [[nodiscard]] auto ticksToNs(std::int64_t deltaTicks) const noexcept -> std::int64_t {
        return static_cast<std::int64_t>(static_cast<double>(deltaTicks) * nsPerTick_);
    }

    [[nodiscard]] auto tickFrequencyGHz() const noexcept -> double {
        return 1.0 / nsPerTick_;
    }
};

/// Simple SIGINT/SIGTERM latch so both tools can be interrupted cleanly
/// instead of being killed mid-write/mid-read.
class SignalHandler {
private:
    alignas(turboq::kCacheLineSize) std::atomic<bool> terminationRequested_{false};

    SignalHandler() = default;

public:
    SignalHandler(SignalHandler const&) = delete;
    SignalHandler& operator=(SignalHandler const&) = delete;

    [[nodiscard]] static auto instance() noexcept -> SignalHandler& {
        static SignalHandler handler;
        return handler;
    }

    [[nodiscard]] auto terminationRequested() const noexcept -> bool {
        return terminationRequested_.load(std::memory_order_relaxed);
    }

    void installSignalHandlers() noexcept {
        std::signal(SIGINT, &handleSignal);
        std::signal(SIGTERM, &handleSignal);
    }

private:
    static void handleSignal(int) noexcept {
        instance().terminationRequested_.store(true, std::memory_order_relaxed);
    }
};

[[nodiscard]] inline auto signalHandler() noexcept -> SignalHandler& {
    return SignalHandler::instance();
}

/// Latency percentile/min/max/mean summary, produced by LatencyCollector::makeReport().
struct Report {
    std::uint64_t received{0};
    std::uint64_t latencyMinNs{0};
    std::uint64_t latencyMaxNs{0};
    double latencyMeanNs{0.0};
    std::uint64_t p50{0};
    std::uint64_t p90{0};
    std::uint64_t p99{0};
    std::uint64_t p999{0};
    std::uint64_t p9999{0};
};

/// Collects per-message latency samples and derives min/max/mean/percentiles. Not thread-safe --
/// each producer/consumer thread should use its own instance.
class LatencyCollector {
private:
    std::vector<std::uint64_t> samples_;

public:
    explicit LatencyCollector(std::size_t reserveHint = 0) {
        samples_.reserve(reserveHint);
    }

    void record(std::uint64_t latencyNs) {
        samples_.push_back(latencyNs);
    }

    [[nodiscard]] auto sampleCount() const noexcept -> std::size_t {
        return samples_.size();
    }

    [[nodiscard]] auto samples() const noexcept -> std::span<std::uint64_t const> {
        return samples_;
    }

    [[nodiscard]] auto makeReport() -> Report {
        Report report;
        report.received = samples_.size();
        if (samples_.empty()) {
            return report;
        }

        std::ranges::sort(samples_);

        report.latencyMinNs = samples_.front();
        report.latencyMaxNs = samples_.back();

        double sum = 0.0;
        for (auto const v : samples_) {
            sum += static_cast<double>(v);
        }
        report.latencyMeanNs = sum / static_cast<double>(samples_.size());

        report.p50 = percentile(0.50);
        report.p90 = percentile(0.90);
        report.p99 = percentile(0.99);
        report.p999 = percentile(0.999);
        report.p9999 = percentile(0.9999);

        return report;
    }

private:
    // Nearest-rank percentile over the (already sorted) sample set.
    [[nodiscard]] auto percentile(double p) const -> std::uint64_t {
        auto const n = samples_.size();
        if (n == 0) {
            return 0;
        }
        auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(n)));
        rank = std::clamp<std::size_t>(rank, 1, n);
        return samples_[rank - 1];
    }
};

inline void printReport(Report const& report) {
    if (report.received == 0) {
        return;
    }
    std::printf("latency (ns):\n");
    std::printf("  min    = %llu\n", static_cast<unsigned long long>(report.latencyMinNs));
    std::printf("  mean   = %.1f\n", report.latencyMeanNs);
    std::printf("  max    = %llu\n", static_cast<unsigned long long>(report.latencyMaxNs));
    std::printf("  p50    = %llu\n", static_cast<unsigned long long>(report.p50));
    std::printf("  p90    = %llu\n", static_cast<unsigned long long>(report.p90));
    std::printf("  p99    = %llu\n", static_cast<unsigned long long>(report.p99));
    std::printf("  p99.9  = %llu\n", static_cast<unsigned long long>(report.p999));
    std::printf("  p99.99 = %llu\n", static_cast<unsigned long long>(report.p9999));
}

/// Validates that message sequence numbers arrive strictly increasing, with no gaps (dropped
/// messages) and no reordering/duplicates. Correct for any single-producer stream (SPSC, or one
/// Multicast producer fanned out to independent consumers) -- each consumer sees a totally
/// ordered stream. NOT valid for a merged multi-producer stream (see CoverageValidator below):
/// with several producers racing, the embedded per-message seq values arrive in reservation
/// order, not in numeric order.
class SequenceValidator {
private:
    std::uint64_t expected_{1};
    std::uint64_t missing_{0};
    std::uint64_t outOfOrder_{0};
    bool first_{true};

public:
    void observe(std::uint64_t seq) {
        if (first_) {
            first_ = false;
            if (seq != 1) {
                std::fprintf(stderr, "WARNING: stream did not start at seq=1 (first seq=%llu)\n",
                    static_cast<unsigned long long>(seq));
            }
            expected_ = seq + 1;
            return;
        }

        if (seq == expected_) {
            ++expected_;
            return;
        }

        if (seq > expected_) {
            auto const missing = seq - expected_;
            missing_ += missing;
            std::fprintf(stderr, "WARNING: sequence gap: expected seq=%llu, got seq=%llu (%llu message(s) lost)\n",
                static_cast<unsigned long long>(expected_), static_cast<unsigned long long>(seq),
                static_cast<unsigned long long>(missing));
            expected_ = seq + 1;
            return;
        }

        // seq < expected_: duplicate or reordering -- both are protocol violations for a single-producer stream
        ++outOfOrder_;
        std::fprintf(stderr, "WARNING: out-of-order/duplicate message: seq=%llu (expected >= %llu)\n",
            static_cast<unsigned long long>(seq), static_cast<unsigned long long>(expected_));
    }

    [[nodiscard]] auto missing() const noexcept -> std::uint64_t {
        return missing_;
    }

    [[nodiscard]] auto outOfOrder() const noexcept -> std::uint64_t {
        return outOfOrder_;
    }

    [[nodiscard]] auto valid() const noexcept -> bool {
        return missing_ == 0 && outOfOrder_ == 0;
    }
};

/// Validates a merged stream from several concurrent producers, each tagged with a seq drawn from
/// its own disjoint [base, base + count) range (see makeGlobalSeq() below). Order between
/// producers is not defined -- only that every seq in [0, total) is delivered exactly once.
class CoverageValidator {
private:
    std::vector<bool> seen_;
    std::uint64_t duplicates_{0};
    std::uint64_t outOfRange_{0};

public:
    explicit CoverageValidator(std::uint64_t total) : seen_(total, false) {}

    void observe(std::uint64_t seq) {
        if (seq >= seen_.size()) {
            ++outOfRange_;
            std::fprintf(stderr, "WARNING: seq=%llu out of expected range [0, %llu)\n",
                static_cast<unsigned long long>(seq), static_cast<unsigned long long>(seen_.size()));
            return;
        }
        if (seen_[seq]) {
            ++duplicates_;
            std::fprintf(stderr, "WARNING: duplicate delivery of seq=%llu\n", static_cast<unsigned long long>(seq));
            return;
        }
        seen_[seq] = true;
    }

    [[nodiscard]] auto missing() const noexcept -> std::uint64_t {
        return static_cast<std::uint64_t>(std::ranges::count(seen_, false));
    }

    [[nodiscard]] auto duplicates() const noexcept -> std::uint64_t {
        return duplicates_;
    }

    [[nodiscard]] auto outOfRange() const noexcept -> std::uint64_t {
        return outOfRange_;
    }

    [[nodiscard]] auto valid() const noexcept -> bool {
        return missing() == 0 && duplicates_ == 0 && outOfRange_ == 0;
    }
};

/// Combines a 0-based producer index with that producer's local message counter into the flat,
/// globally-unique [0, producerCount * perProducerCount) range CoverageValidator expects.
[[nodiscard]] constexpr auto makeGlobalSeq(
    unsigned producerId, std::uint64_t localIndex, std::uint64_t perProducerCount) noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(producerId) * perProducerCount + localIndex;
}

} // namespace bench
