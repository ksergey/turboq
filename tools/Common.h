// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0
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
