// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <algorithm>
#include <atomic>
#include <bit>
#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <cxxopts.hpp>

#include <turboq/MulticastQueue.h>

#include "Common.h"

struct Config {
    std::string queueName;
    std::uint64_t idle;
    std::uint64_t count;
};

struct Report {
    std::uint64_t received;
    std::uint64_t latencyMin;
    std::uint64_t latencyMax;
    std::uint64_t latencyMean;
    std::uint64_t p01;
    std::uint64_t p50;
    std::uint64_t p99;
    std::uint64_t p999;
    std::uint64_t p9999;
};

class Collector {
private:
    std::vector<std::uint64_t> storage_;
    std::uint64_t firstSeq_{0};
    std::uint64_t lastSeq_{0};

public:
    explicit Collector(std::size_t reserve) {
        storage_.reserve(reserve);
    }

    void record(std::uint64_t seq, std::uint64_t latency) {
        if (storage_.empty()) [[unlikely]] {
            firstSeq_ = seq;
        }
        lastSeq_ = seq;
        storage_.push_back(latency);
    }

    auto makeReport() -> Report {
        if (storage_.empty()) {
            return {};
        }

        std::ranges::sort(storage_);

        Report report{};
        report.received = storage_.size();
        report.latencyMin = storage_.front();
        report.latencyMax = storage_.back();
        report.latencyMean = std::ranges::fold_left(storage_, 0.0, std::plus{}) / static_cast<double>(storage_.size());
        report.p01 = percentile(storage_, 0.01);
        report.p50 = percentile(storage_, 0.50);
        report.p99 = percentile(storage_, 0.99);
        report.p999 = percentile(storage_, 0.999);
        report.p9999 = percentile(storage_, 0.9999);

        return report;
    }

private:
    static auto percentile(const std::vector<uint64_t>& sorted, double p) -> std::uint64_t {
        if (sorted.empty()) {
            return 0;
        }

        auto const n = sorted.size();
        auto rank = static_cast<std::size_t>(p * static_cast<double>(n));
        if (static_cast<double>(rank) < p * static_cast<double>(n)) {
            ++rank;
        }
        if (rank < 1) {
            rank = 1;
        }
        if (rank > n) {
            rank = n;
        }
        return sorted[rank - 1];
    }
};

void run(Config const& cfg) {
    auto consumer = turboq::MulticastQueue{cfg.queueName}.createConsumer();

    std::printf("consumer (multicast): queue=%s, capacity=%llu, count=%llu\n", cfg.queueName.c_str(),
        static_cast<unsigned long long>(consumer.capacity()), static_cast<unsigned long long>(cfg.count));

    auto const idleInterval = cfg.idle * 1000000ull;

    Collector collector{cfg.count};

    std::uint64_t received = 0;
    std::uint64_t lastUpdate = common::clockNow();

    while (received < cfg.count) {
        auto const rc = common::consume(consumer, [&](auto buffer) {
            auto const now = common::clockNow();
            auto const msg = std::bit_cast<common::Message const*>(buffer.data());
            auto const latency = now - msg->sendingTime;

            collector.record(msg->seq, latency);

            ++received;
            lastUpdate = now;
        });
        if (!rc) {
            if (common::clockNow() - lastUpdate > idleInterval) {
                break;
            }
        }
    }

    auto const report = collector.makeReport();
    std::printf("received %llu messages\n", static_cast<unsigned long long>(report.received));
    std::printf("min     = %lluns\n", static_cast<unsigned long long>(report.latencyMin));
    std::printf("max     = %lluns\n", static_cast<unsigned long long>(report.latencyMax));
    std::printf("mean    = %lluns\n", static_cast<unsigned long long>(report.latencyMean));
    std::printf("p01     = %lluns\n", static_cast<unsigned long long>(report.p01));
    std::printf("p50     = %lluns\n", static_cast<unsigned long long>(report.p50));
    std::printf("p99     = %lluns\n", static_cast<unsigned long long>(report.p99));
    std::printf("p99.9   = %lluns\n", static_cast<unsigned long long>(report.p999));
    std::printf("p99.99  = %lluns\n", static_cast<unsigned long long>(report.p9999));
}

int main(int argc, char* argv[]) {
    try {
        auto options = cxxopts::Options{"consumer", ""};
        options.set_width(120);
        options.set_tab_expansion();

        // clang-format off
        options.add_options()
            ("n,name", "queue name", cxxopts::value<std::string>()->default_value("fanout"))
            ("i,idle", "max idle time before exit (ms)", cxxopts::value<std::uint64_t>()->default_value("2000"))
            ("c,count", "number of messages to read", cxxopts::value<std::uint64_t>()->default_value("100000"))
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
        cfg.idle = args["idle"].as<std::uint64_t>();
        cfg.count = args["count"].as<std::uint64_t>();

        run(cfg);

    } catch (std::exception const& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
