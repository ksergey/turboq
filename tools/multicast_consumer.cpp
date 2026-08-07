// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0
//
// Reads messages from a turboq multicast queue, validates that the sequence
// numbers arrive strictly consecutive (no gaps, no reordering/duplicates),
// and reports min/max/mean/percentile latency at the end.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <cxxopts.hpp>

#include <turboq/MulticastQueue.h>

#include "Common.h"

namespace {

struct Config {
    std::string queueName;
    std::uint64_t count;
    std::uint64_t idleMs;
};

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

/// Collects per-message latency samples and derives min/max/mean/percentiles.
class LatencyCollector {
private:
    std::vector<std::uint64_t> samples_;

public:
    explicit LatencyCollector(std::size_t reserveHint) {
        samples_.reserve(reserveHint);
    }

    void record(std::uint64_t latencyNs) {
        samples_.push_back(latencyNs);
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

/// Validates that message sequence numbers arrive strictly increasing, with
/// no gaps (dropped/never-sent messages) and no reordering/duplicates. For an
/// multicast queue this should always hold in practice; violations indicate either
/// a bug in the queue or two producers/consumers racing on the same name.
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

        // seq < expected_: duplicate or reordering -- both are protocol violations for an multicast stream
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

void printReport(Report const& report, Config const& cfg, double elapsedSec, SequenceValidator const& validator) {
    std::printf("\n--- results ---\n");
    std::printf("received       : %llu / %llu requested\n", static_cast<unsigned long long>(report.received),
        static_cast<unsigned long long>(cfg.count));
    std::printf("elapsed        : %.3f sec (%.0f msg/s)\n", elapsedSec,
        elapsedSec > 0 ? static_cast<double>(report.received) / elapsedSec : 0.0);
    std::printf("sequence check : %s (missing=%llu, out-of-order/duplicate=%llu)\n",
        validator.valid() ? "OK" : "FAILED", static_cast<unsigned long long>(validator.missing()),
        static_cast<unsigned long long>(validator.outOfOrder()));

    if (report.received == 0) {
        return;
    }

    std::printf("\nlatency (ns):\n");
    std::printf("  min    = %llu\n", static_cast<unsigned long long>(report.latencyMinNs));
    std::printf("  mean   = %.1f\n", report.latencyMeanNs);
    std::printf("  max    = %llu\n", static_cast<unsigned long long>(report.latencyMaxNs));
    std::printf("  p50    = %llu\n", static_cast<unsigned long long>(report.p50));
    std::printf("  p90    = %llu\n", static_cast<unsigned long long>(report.p90));
    std::printf("  p99    = %llu\n", static_cast<unsigned long long>(report.p99));
    std::printf("  p99.9  = %llu\n", static_cast<unsigned long long>(report.p999));
    std::printf("  p99.99 = %llu\n", static_cast<unsigned long long>(report.p9999));
}

/// Returns true if the run is "clean": expected message count received and no
/// sequence violations. Used as the process exit status.
auto run(Config const& cfg) -> bool {
    auto consumer = turboq::MulticastQueue{cfg.queueName}.createConsumer();

    std::printf("consumer: queue=%s capacity=%llu bytes, expecting=%llu messages, idle timeout=%llu ms\n",
        cfg.queueName.c_str(), static_cast<unsigned long long>(consumer.capacity()),
        static_cast<unsigned long long>(cfg.count), static_cast<unsigned long long>(cfg.idleMs));

    bench::signalHandler().installSignalHandlers();

    LatencyCollector collector{cfg.count};
    SequenceValidator validator;

    auto const idleThresholdNs = static_cast<std::int64_t>(cfg.idleMs) * 1'000'000;
    auto const startTime = bench::clockNow();
    auto lastActivity = startTime;

    std::uint64_t received = 0;
    while (received < cfg.count && !bench::signalHandler().terminationRequested()) {
        auto buffer = consumer.fetch();
        if (buffer.empty()) {
            if (bench::clockNow() - lastActivity > idleThresholdNs) {
                std::fprintf(stderr, "WARNING: idle timeout reached, stopping early (%llu/%llu received)\n",
                    static_cast<unsigned long long>(received), static_cast<unsigned long long>(cfg.count));
                break;
            }
            continue; // spin-wait: lowest latency, matches the queue's low-latency design
        }

        auto const now = bench::clockNow();
        auto const* msg = std::bit_cast<bench::Message const*>(buffer.data());

        validator.observe(msg->seq);
        collector.record(static_cast<std::uint64_t>(now - msg->sendTimeNs));

        consumer.consume();
        ++received;
        lastActivity = now;
    }

    auto const elapsedSec = static_cast<double>(bench::clockNow() - startTime) / 1e9;
    auto const report = collector.makeReport();

    if (bench::signalHandler().terminationRequested()) {
        std::printf("interrupted\n");
    }

    printReport(report, cfg, elapsedSec, validator);

    return validator.valid() && report.received == cfg.count;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        cxxopts::Options options{
            "multicast_consumer", "Multicast queue consumer - validates sequence, reports latency stats"};
        options.set_width(120);
        options.set_tab_expansion();

        // clang-format off
        options.add_options()
            ("n,name", "queue name", cxxopts::value<std::string>()->default_value("turboq.bench"))
            ("m,count", "number of messages to read before exiting", cxxopts::value<std::uint64_t>()->default_value("1000000"))
            ("i,idle", "max idle time with no data before giving up early (ms)", cxxopts::value<std::uint64_t>()->default_value("5000"))
            ("h,help", "print help and exit")
        ;
        // clang-format on

        auto args = options.parse(argc, argv);
        if (args.count("help")) {
            std::fprintf(stdout, "%s\n", options.help().c_str());
            return EXIT_SUCCESS;
        }

        Config cfg;
        cfg.queueName = args["name"].as<std::string>();
        cfg.count = args["count"].as<std::uint64_t>();
        cfg.idleMs = args["idle"].as<std::uint64_t>();

        if (cfg.count == 0) {
            std::fprintf(stderr, "ERROR: --count must be > 0\n");
            return EXIT_FAILURE;
        }

        auto const ok = run(cfg);
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;

    } catch (std::exception const& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return EXIT_FAILURE;
    }
}
