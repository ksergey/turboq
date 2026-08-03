// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include <turboq/BoundedMPSCRawQueue.h>
#include <turboq/BoundedMulticastRawQueue.h>
#include <turboq/BoundedSPSCRawQueue.h>

#include <cxxopts.hpp>

#include "common.h"

enum class QueueType { Multicast, SPSC, MPSC };

struct QueueConfig {
    std::string queueName;
    QueueType queueType;
    turboq::HugePagesOption hugePagesOption;
    std::size_t capacityHint;
    std::size_t lengthHint;
};

enum class Kind { Trade, BBO, Book, Mixed };

struct RunConfig {
    double rate;
    Kind kind;
};

auto parseConfigsFromCliArgs(QueueConfig& queueConfig, RunConfig& runConfig, int argc, char* argv[]) -> int {
    auto options = cxxopts::Options{"producer"};
    options.set_width(120);
    options.set_tab_expansion();

    // clang-format off
    options.add_options()
        ("n,queue-name", "queue name", cxxopts::value<std::string>()->default_value("fanout"))
        ("t,queue-type", "queue type (multicast|spsc|mpsc)", cxxopts::value<std::string>()->default_value("multicast"))
        ("p,huge-pages", "use huge pages (auto|none|2m|1g)", cxxopts::value<std::string>()->default_value("none"))
        ("h,help", "print help and exit")
    ;
    // clang-format on

    // clang-format off
    auto multicastQueueOptions = options.add_options("multicast/spsc queue options")
        ("c,capacity", "queue capacity hint (Mb)", cxxopts::value<std::size_t>()->default_value("32"))
    ;
    // clang-format on

    // clang-format off
    auto mpscQueueOptions = options.add_options("mpsc queue options")
        ("l,length", "queue length hint", cxxopts::value<std::size_t>()->default_value("1000000"))
    ;
    // clang-format on

    // clang-format off
    auto runOptions = options.add_options("run config")
        ("r,rate", "publish rate", cxxopts::value<double>()->default_value("0.0"))
        ("k,kind", "publish data kind (trade|bbo|book|mixed)", cxxopts::value<std::string>()->default_value("mixed"))
    ;
    // clang-format on

    auto args = options.parse(argc, argv);

    if (args.count("help")) {
        std::fprintf(stdout, "%s\n", options.help().c_str());
        return 1;
    }

    queueConfig.queueName = args["queue-name"].as<std::string>();

    auto const& queueTypeStr = args["queue-type"].as<std::string>();
    if (queueTypeStr == "multicast") {
        queueConfig.queueType = QueueType::Multicast;
    } else if (queueTypeStr == "spsc") {
        queueConfig.queueType = QueueType::SPSC;
    } else if (queueTypeStr == "mpsc") {
        queueConfig.queueType = QueueType::MPSC;
    } else {
        std::fprintf(stderr, "unknown queue type %s\n", queueTypeStr.c_str());
        return -1;
    }

    auto const& hugePagesOptionStr = args["huge-pages"].as<std::string>();
    if (hugePagesOptionStr == "none") {
        queueConfig.hugePagesOption = turboq::HugePagesOption::None;
    } else if (hugePagesOptionStr == "auto") {
        queueConfig.hugePagesOption = turboq::HugePagesOption::Auto;
    } else if (hugePagesOptionStr == "2m") {
        queueConfig.hugePagesOption = turboq::HugePagesOption::HugePages2M;
    } else if (hugePagesOptionStr == "1g") {
        queueConfig.hugePagesOption = turboq::HugePagesOption::HugePages1G;
    } else {
        std::fprintf(stderr, "unknown huge pages option value %s\n", hugePagesOptionStr.c_str());
        return -1;
    }

    queueConfig.capacityHint = args["capacity"].as<std::size_t>() * 1024 * 1024;
    queueConfig.lengthHint = args["length"].as<std::size_t>();

    runConfig.rate = args["rate"].as<double>();

    auto const& kindStr = args["kind"].as<std::string>();
    if (kindStr == "trade") {
        runConfig.kind = Kind::Trade;
    } else if (kindStr == "bbo") {
        runConfig.kind = Kind::BBO;
    } else if (kindStr == "book") {
        runConfig.kind = Kind::Book;
    } else if (kindStr == "mixed") {
        runConfig.kind = Kind::Mixed;
    } else {
        std::fprintf(stderr, "unknown kind value %s\n", kindStr.c_str());
        return -1;
    }

    return 0;
}

[[nodiscard]] constexpr auto toString(QueueType value) noexcept -> char const* {
    switch (value) {
    case QueueType::Multicast: return "multicast";
    case QueueType::SPSC: return "spsc";
    case QueueType::MPSC: return "mpsc";
    }
    return "";
}

[[nodiscard]] constexpr auto toString(turboq::HugePagesOption value) noexcept -> char const* {
    switch (value) {
    case turboq::HugePagesOption::Auto: return "auto";
    case turboq::HugePagesOption::HugePages2M: return "2m";
    case turboq::HugePagesOption::HugePages1G: return "1g";
    case turboq::HugePagesOption::None: return "none";
    }
    return "";
}

[[nodiscard]] constexpr auto toString(Kind value) noexcept -> char const* {
    switch (value) {
    case Kind::Trade: return "trade";
    case Kind::BBO: return "bbo";
    case Kind::Book: return "book";
    case Kind::Mixed: return "mixed";
    }
    return "";
}

void describe(QueueConfig const& queueConfig, RunConfig const& runConfig) noexcept {
    switch (queueConfig.queueType) {
    case QueueType::Multicast:
    case QueueType::SPSC: {
        std::printf("producer: queue-name=%s, queue-type=%s, capacity-hint=%llu, huge-pages=%s, rate=%lf, kind=%s\n",
            queueConfig.queueName.c_str(), toString(queueConfig.queueType),
            static_cast<unsigned long long>(queueConfig.capacityHint), toString(queueConfig.hugePagesOption),
            runConfig.rate, toString(runConfig.kind));
    } break;
    case QueueType::MPSC: {
        std::printf("producer: queue-name=%s, queue-type=%s, length-hint=%llu, huge-pages=%s, rate=%lf, kind=%s\n",
            queueConfig.queueName.c_str(), toString(queueConfig.queueType),
            static_cast<unsigned long long>(queueConfig.lengthHint), toString(queueConfig.hugePagesOption),
            runConfig.rate, toString(runConfig.kind));
    } break;
    }
}

template <typename Producer>
auto run(Producer& producer, RunConfig const& cfg) -> int {
    (void)producer;
    (void)cfg;
    return 0;
}

int main(int argc, char* argv[]) {
    try {
        QueueConfig queueConfig;
        RunConfig runConfig;

        if (auto const rc = parseConfigsFromCliArgs(queueConfig, runConfig, argc, argv); rc != 0) {
            return EXIT_FAILURE;
        }

        describe(queueConfig, runConfig);

        switch (queueConfig.queueType) {
        case QueueType::Multicast: {
            auto producer = turboq::BoundedMulticastRawQueue{queueConfig.queueName,
                turboq::BoundedMulticastRawQueue::CreationOptions{.capacityHint = queueConfig.capacityHint},
                turboq::DefaultMemorySource{queueConfig.hugePagesOption}}
                                .createProducer();
            return run(producer, runConfig);
        } break;
        case QueueType::SPSC: {
            auto producer = turboq::BoundedSPSCRawQueue{queueConfig.queueName,
                turboq::BoundedSPSCRawQueue::CreationOptions{.capacityHint = queueConfig.capacityHint},
                turboq::DefaultMemorySource{queueConfig.hugePagesOption}}
                                .createProducer();
            return run(producer, runConfig);
        } break;
        case QueueType::MPSC: {
            auto producer = turboq::BoundedMPSCRawQueue{queueConfig.queueName,
                turboq::BoundedMPSCRawQueue::CreationOptions{
                    .maxMessageSizeHint = 999, .lengthHint = queueConfig.lengthHint},
                turboq::DefaultMemorySource{queueConfig.hugePagesOption}}
                                .createProducer();
            return run(producer, runConfig);
        } break;
        }

    } catch (std::exception const& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
