// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <atomic>
#include <bit>
#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>

#include <cxxopts.hpp>

#include <turboq/MulticastQueue.h>

#include "Common.h"

class SignalHandler {
private:
    alignas(turboq::kCacheLineSize) std::atomic<bool> terminationRequested_{false};

    SignalHandler() = default;

public:
    SignalHandler(SignalHandler const&) = delete;
    SignalHandler& operator=(SignalHandler const&) = delete;

    [[nodiscard]] static auto instance() -> SignalHandler& {
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
    static void handleSignal(int) {
        instance().terminationRequested_.store(true, std::memory_order_relaxed);
    }
};

auto signalHandler() noexcept -> SignalHandler& {
    return SignalHandler::instance();
}

struct Config {
    std::string queueName;
    std::size_t capacityHint;
    double rate;
};

void run(Config const& cfg) {
    auto producer =
        turboq::MulticastQueue{cfg.queueName, turboq::MulticastQueue::CreationOptions{.capacityHint = cfg.capacityHint}}
            .createProducer();

    auto const interval = (cfg.rate > 0) ? static_cast<std::uint64_t>(1e9 / cfg.rate) : 0u;
    auto deadline = common::clockNow();

    std::printf("producer (multicast): queue=%s, capacity=%llu, rate=%.0f\n", cfg.queueName.c_str(),
        static_cast<unsigned long long>(producer.capacity()), cfg.rate);

    std::uint64_t seq = 0;
    while (!signalHandler().terminationRequested()) {
        if (interval) {
            while (common::clockNow() < deadline) {
                // TODO: yield? pause?
            }
            deadline += interval;
        }
        ++seq;
        common::publish(producer, sizeof(common::Message), [&](auto buffer) {
            auto const msg = std::bit_cast<common::Message*>(buffer.data());
            msg->sendingTime = common::clockNow();
            msg->seq = seq;
        });
    }

    std::printf("interrupted\n");
    std::printf("sent %llu messages\n", static_cast<unsigned long long>(seq));
}

int main(int argc, char* argv[]) {
    try {
        auto options = cxxopts::Options{"producer", ""};
        options.set_width(120);
        options.set_tab_expansion();

        // clang-format off
        options.add_options()
            ("n,name", "queue name", cxxopts::value<std::string>()->default_value("fanout"))
            ("c,capacity", "queue capacity hint (mb)", cxxopts::value<std::size_t>()->default_value("32"))
            ("r,rate", "msgs per second (0 - no cap)", cxxopts::value<double>()->default_value("0"))
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
        cfg.capacityHint = args["capacity"].as<std::size_t>();
        cfg.rate = args["rate"].as<double>();

        signalHandler().installSignalHandlers();

        run(cfg);

    } catch (std::exception const& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
