// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0
//
// Writes messages into a turboq Multicast queue at a configurable rate.
// Each message carries a monotonically increasing sequence number and a
// send timestamp so the consumer can validate ordering and measure latency.

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <cxxopts.hpp>

#include <turboq/MulticastQueue.h>

#include "Common.h"

namespace {

struct Config {
    std::string queueName;
    std::size_t capacityBytes;
    std::size_t messageSize;
    double rate; // messages per second, 0 = unlimited
};

/// Busy/sleep hybrid wait until 'deadlineNs'. Sleeping for long waits avoids
/// burning a full core; the last stretch is spun to keep pacing tight.
void waitUntil(std::int64_t deadlineNs) noexcept {
    constexpr std::int64_t kSpinThresholdNs = 200'000; // 200us
    for (;;) {
        auto const now = bench::clockNow();
        if (now >= deadlineNs) {
            return;
        }
        auto const remaining = deadlineNs - now;
        if (remaining > kSpinThresholdNs) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(remaining - kSpinThresholdNs));
        }
        // else: busy-spin through the last sub-200us stretch for precise pacing
    }
}

void run(Config const& cfg) {
    auto producer = turboq::MulticastQueue{cfg.queueName,
        turboq::MulticastQueue::CreationOptions{.capacityHint = cfg.capacityBytes}}
                        .createProducer();

    auto const rateStr =
        cfg.rate > 0 ? (std::to_string(static_cast<long long>(cfg.rate)) + "/s") : std::string{"unlimited"};
    std::printf("multicast_producer: queue=%s capacity=%llu bytes, message size=%llu bytes, rate=%s\n",
        cfg.queueName.c_str(), static_cast<unsigned long long>(producer.capacity()),
        static_cast<unsigned long long>(cfg.messageSize), rateStr.c_str());

    auto const intervalNs = cfg.rate > 0 ? static_cast<std::int64_t>(1e9 / cfg.rate) : 0;
    auto deadline = bench::clockNow();

    bench::signalHandler().installSignalHandlers();

    std::uint64_t sent = 0;
    std::uint64_t retries = 0;
    auto const startTime = bench::clockNow();

    while (!bench::signalHandler().terminationRequested()) {
        if (intervalNs > 0) {
            waitUntil(deadline);
            deadline += intervalNs;
        }

        std::span<std::byte> buffer;
        for (;;) {
            buffer = producer.prepare(cfg.messageSize);
            if (!buffer.empty()) {
                break;
            }
            // queue full: consumer is lagging behind, spin-wait for it to catch up
            ++retries;
            if (bench::signalHandler().terminationRequested()) {
                std::printf("interrupted while waiting for free space\n");
                return;
            }
        }

        if (buffer.size() > sizeof(bench::Message)) {
            std::memset(buffer.data() + sizeof(bench::Message), 0xAB, buffer.size() - sizeof(bench::Message));
        }

        auto* msg = std::bit_cast<bench::Message*>(buffer.data());
        msg->seq = sent + 1;
        // capture the timestamp as close to commit() as possible to minimize measurement noise
        msg->sendTimeNs = bench::clockNow();

        producer.commit();
        ++sent;
    }

    auto const elapsedNs = bench::clockNow() - startTime;
    auto const elapsedSec = static_cast<double>(elapsedNs) / 1e9;

    if (bench::signalHandler().terminationRequested()) {
        std::printf("interrupted\n");
    }
    std::printf("sent %llu messages in %.3f sec (retries=%llu, effective rate=%.0f msg/s)\n",
        static_cast<unsigned long long>(sent), elapsedSec, static_cast<unsigned long long>(retries),
        elapsedSec > 0 ? static_cast<double>(sent) / elapsedSec : 0.0);
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        cxxopts::Options options{
            "multicast_producer", "Multicast queue producer - writes messages at a configurable rate"};
        options.set_width(120);
        options.set_tab_expansion();

        // clang-format off
        options.add_options()
            ("n,name", "queue name", cxxopts::value<std::string>()->default_value("turboq.bench"))
            ("c,capacity", "queue capacity hint (MiB)", cxxopts::value<std::size_t>()->default_value("64"))
            ("s,size", "message size in bytes, including the sequence/timestamp header (min 16)",
                cxxopts::value<std::size_t>()->default_value("64"))
            ("r,rate", "messages per second (0 = as fast as possible)", cxxopts::value<double>()->default_value("0"))
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
        cfg.capacityBytes = args["capacity"].as<std::size_t>() * 1024ull * 1024ull;
        cfg.messageSize = args["size"].as<std::size_t>();
        cfg.rate = args["rate"].as<double>();

        if (cfg.messageSize < sizeof(bench::Message)) {
            std::fprintf(stderr, "ERROR: --size must be >= %zu bytes (message header size)\n", sizeof(bench::Message));
            return EXIT_FAILURE;
        }
        if (cfg.rate < 0) {
            std::fprintf(stderr, "ERROR: --rate must be >= 0\n");
            return EXIT_FAILURE;
        }

        run(cfg);

    } catch (std::exception const& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
