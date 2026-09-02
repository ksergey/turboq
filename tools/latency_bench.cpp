// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT
//
// Single binary, single role per run: pass --role producer or --role consumer,
// and --type spsc|mpsc|multicast to pick which queue implementation to talk
// to. Meant to be launched twice from the shell -- once as producer, once as
// consumer -- exactly like a pair of separate tools, just without maintaining
// two near-identical source files per queue type.
//
//   $ ./latency_bench --role producer --type spsc --name q --count 1000000 &
//   $ ./latency_bench --role consumer --type spsc --name q --count 1000000
//
// The producer must be started first: it creates the backing queue (via
// DefaultMemorySource, a real file so a separate process can find it by
// name); the consumer only opens what's already there. For multicast, the
// consumer can be started (and stopped, and started again as a fresh
// subscriber) any number of times independently -- it only ever sees
// messages published after it attaches. For mpsc, --role producer can be run
// concurrently more than once against the same --name to exercise multiple
// producers; give each instance a distinct --producer-id so the consumer can
// tell their sequences apart (see --producers below).

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <cxxopts.hpp>

#include <turboq/MPSCMessageQueue.h>
#include <turboq/MemorySource.h>
#include <turboq/MulticastMessageQueue.h>
#include <turboq/SPSCMessageQueue.h>
#include <turboq/Utils.h>

#include "Common.h"

namespace {

struct BenchmarkError : public std::runtime_error {
    using std::runtime_error::runtime_error;

    template <typename... Args>
    BenchmarkError(std::format_string<Args...> fmt, Args&&... args)
        : std::runtime_error{std::format(fmt, std::forward<Args>(args)...)} {}
};

enum class Role { Producer, Consumer };
enum class QueueType { SPSC, MPSC, Multicast };

struct Config {
    Role role;
    QueueType type;
    std::string queueName;
    std::size_t capacityBytes; // spsc / multicast
    std::size_t lengthHint;    // mpsc ring length hint, in slots
    std::size_t messageSize;
    std::uint64_t count;               // producer: messages to send; consumer: messages to expect per producer
    double rate;                       // producer only: messages per second (0 = unlimited)
    unsigned producerId;               // producer only: this instance's slot in the global sequence range (mpsc)
    unsigned producerCount;            // consumer only: how many distinct producer-id ranges to expect (mpsc)
    std::uint64_t warmup;              // consumer only: messages excluded from latency stats (still validated)
    std::uint64_t idleMs;              // consumer only
    turboq::HugePagesOption hugePages; // where the queue's backing memory is allocated (see --hugepages);
                                       // producer and consumer must agree on this, or they'll resolve to
                                       // different mount points and the consumer won't find the queue
    bool drain;                        // consumer only: drain queue before start
    bool openOnly;                     // open an existing queue without creating/initializing it (either role)
    bench::ClockSource clockSource;    // per-message timestamp source; must match between producer and
                                       // consumer or the "latency" numbers are meaningless (see --clock)
};

[[nodiscard]] auto parseClockSource(std::string const& value) -> bench::ClockSource {
    if (value == "wall") {
        return bench::ClockSource::Wall;
    }
    if (value == "rdtsc") {
        return bench::ClockSource::Tsc;
    }
    throw BenchmarkError{"unknown --clock \"{}\" (expected wall or rdtsc)", value};
}

[[nodiscard]] auto parseHugePages(std::string const& value) -> turboq::HugePagesOption {
    if (value == "auto") {
        return turboq::HugePagesOption::Auto;
    }
    if (value == "none") {
        return turboq::HugePagesOption::None;
    }
    if (value == "2m") {
        return turboq::HugePagesOption::HugePages2M;
    }
    if (value == "1g") {
        return turboq::HugePagesOption::HugePages1G;
    }
    throw BenchmarkError{"unknown --hugepages \"{}\" (expected auto, none, 2m or 1g)", value};
}

/// DefaultMemorySource{HugePagesOption} throws a bare ENOENT ("No such file or directory") when it
/// can't find a hugetlbfs mount for the requested page size -- correct, but not obviously
/// actionable if you don't already know that's what it's checking for. Add a pointer to what to
/// actually go fix (see README.md's "Huge pages" section for the actual reservation/mount steps).
[[nodiscard]] auto makeMemorySource(Config const& cfg) -> turboq::DefaultMemorySource {
    try {
        return turboq::DefaultMemorySource{cfg.hugePages};
    } catch (std::system_error const& e) {
        char const* optValue = [&] {
            switch (cfg.hugePages) {
            case turboq::HugePagesOption::Auto: return "auto";
            case turboq::HugePagesOption::None: return "none";
            case turboq::HugePagesOption::HugePages2M: return "2m";
            case turboq::HugePagesOption::HugePages1G: return "1g";
            }
            return "?";
        }();

        throw BenchmarkError{"{} -- no hugetlbfs mount found for --hugepages={}. Huge pages need to be reserved and "
                             "mounted first; see README.md's \"Huge pages\" section.",
            e.what(), optValue};
    }
}

[[nodiscard]] auto parseRole(std::string const& value) -> Role {
    if (value == "producer") {
        return Role::Producer;
    }
    if (value == "consumer") {
        return Role::Consumer;
    }
    throw BenchmarkError{"unknown --role \"{}\" (expected producer or consumer)", value};
}

[[nodiscard]] auto parseQueueType(std::string const& value) -> QueueType {
    if (value == "spsc") {
        return QueueType::SPSC;
    }
    if (value == "mpsc") {
        return QueueType::MPSC;
    }
    if (value == "multicast") {
        return QueueType::Multicast;
    }
    throw BenchmarkError{"unknown --type \"{}\" (expected spsc, mpsc or multicast)", value};
}

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
        } else {
            turboq::cpuRelax(); // busy-spin through the last sub-200us stretch for precise pacing
        }
    }
}

/// Shared producer loop: works for any turboq Producer (SPSC/MPSC/Multicast all satisfy the same
/// prepare()/commit() concept). seq numbers are 1-based and offset by producerId * count, so a
/// consumer merging several mpsc producer processes can tell them apart (see --producer-id).
template <typename ProducerT>
void runProducer(ProducerT& producer, Config const& cfg) {
    auto const intervalNs = cfg.rate > 0 ? static_cast<std::int64_t>(1e9 / cfg.rate) : 0;
    auto const seqBase = static_cast<std::uint64_t>(cfg.producerId) * cfg.count;

    std::printf("producer: queue=%s message size=%llu bytes, count=%llu, rate=%s%s, clock=%s\n", cfg.queueName.c_str(),
        static_cast<unsigned long long>(cfg.messageSize), static_cast<unsigned long long>(cfg.count),
        cfg.rate > 0 ? (std::to_string(static_cast<long long>(cfg.rate)) + "/s").c_str() : "unlimited",
        cfg.producerId != 0 ? (" producer-id=" + std::to_string(cfg.producerId)).c_str() : "",
        cfg.clockSource == bench::ClockSource::Tsc ? "rdtsc" : "wall");

    bench::signalHandler().installSignalHandlers();

    auto deadline = bench::clockNow();
    std::uint64_t sent = 0;
    std::uint64_t retries = 0;
    auto const startTime = bench::clockNow();

    while (sent < cfg.count && !bench::signalHandler().terminationRequested()) {
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
            ++retries; // ring full: the consumer is lagging behind, spin-wait for it to catch up
            if (bench::signalHandler().terminationRequested()) {
                std::printf("interrupted while waiting for free space\n");
                return;
            }
            turboq::cpuRelax();
        }

        if (buffer.size() > sizeof(bench::Message)) {
            std::memset(buffer.data() + sizeof(bench::Message), 0xAB, buffer.size() - sizeof(bench::Message));
        }

        auto* msg = std::bit_cast<bench::Message*>(buffer.data());
        msg->seq = seqBase + sent + 1;
        // capture the timestamp as close to commit() as possible to minimize measurement noise
        msg->sendTimeNs = cfg.clockSource == bench::ClockSource::Tsc ? static_cast<std::int64_t>(bench::readTsc())
                                                                     : bench::clockNow();

        producer.commit();
        ++sent;
    }

    auto const elapsedSec = static_cast<double>(bench::clockNow() - startTime) / 1e9;

    if (bench::signalHandler().terminationRequested()) {
        std::printf("interrupted\n");
    }
    std::printf("sent %llu messages in %.3f sec (retries=%llu, effective rate=%.0f msg/s)\n",
        static_cast<unsigned long long>(sent), elapsedSec, static_cast<unsigned long long>(retries),
        elapsedSec > 0 ? static_cast<double>(sent) / elapsedSec : 0.0);
}

/// Shared consumer loop: works for any turboq Consumer. For a single producer (the default,
/// producerCount=1) sequence numbers are strictly 1, 2, 3, ... and SequenceValidator catches any
/// gap/reorder/duplicate. For several mpsc producers (--producers > 1) delivery order between
/// producers is unspecified by design, so each producer's own sub-range is validated separately.
template <typename ConsumerT>
auto runConsumer(ConsumerT& consumer, Config const& cfg) -> bool {
    auto const totalExpected = static_cast<std::uint64_t>(cfg.producerCount) * cfg.count;

    std::printf("consumer: queue=%s expecting=%llu messages%s, idle timeout=%llu ms, clock=%s\n", cfg.queueName.c_str(),
        static_cast<unsigned long long>(totalExpected),
        cfg.producerCount != 1 ? (" (" + std::to_string(cfg.producerCount) + " producers)").c_str() : "",
        static_cast<unsigned long long>(cfg.idleMs), cfg.clockSource == bench::ClockSource::Tsc ? "rdtsc" : "wall");

    bench::signalHandler().installSignalHandlers();

    // Only pay the ~50ms calibration cost when it's actually needed. This must happen on THIS
    // process -- the tick rate isn't something the producer can hand over, and each process's own
    // calibration is what its own readTsc() deltas need to be divided against.
    std::optional<bench::TscCalibration> tscCalibration;
    if (cfg.clockSource == bench::ClockSource::Tsc) {
        tscCalibration.emplace();
        std::printf("calibrated TSC frequency: %.3f GHz\n", tscCalibration->tickFrequencyGHz());
    }

    bench::LatencyCollector collector{totalExpected >= cfg.warmup ? totalExpected - cfg.warmup : 0};
    std::vector<bench::SequenceValidator> validators(cfg.producerCount); // one strict validator per producer-id range

    // Skip whatever's already buffered and start from "now": reset() snapshots the producer's
    // current position once, rather than looping fetch()/consume() (which would race an actively
    // writing producer, with no upper bound on how much it could silently discard, and no
    // termination-signal check either).
    if (cfg.drain) {
        consumer.reset();
    }

    auto const idleThresholdNs = static_cast<std::int64_t>(cfg.idleMs) * 1'000'000;
    auto const startTime = bench::clockNow();
    auto lastActivity = startTime;

    std::uint64_t received = 0;
    while (received < totalExpected && !bench::signalHandler().terminationRequested()) {
        auto buffer = consumer.fetch();
        if (buffer.empty()) {
            if (bench::clockNow() - lastActivity > idleThresholdNs) {
                std::fprintf(stderr, "WARNING: idle timeout reached, stopping early (%llu/%llu received)\n",
                    static_cast<unsigned long long>(received), static_cast<unsigned long long>(totalExpected));
                break;
            }
            turboq::cpuRelax(); // spin-wait: lowest latency, matches the queue's low-latency design
            continue;
        }

        auto const nowWall = bench::clockNow();
        auto const* msg = std::bit_cast<bench::Message const*>(buffer.data());

        // msg->seq is 1-based and offset by producerId * cfg.count (see runProducer); recover which
        // producer's range it falls in and validate that sub-range independently.
        auto const zeroBased = msg->seq - 1;
        auto const producerId = zeroBased / cfg.count;
        auto const localSeq = (zeroBased % cfg.count) + 1;
        if (producerId < validators.size()) {
            validators[producerId].observe(localSeq);
        } else {
            std::fprintf(stderr, "WARNING: message seq=%llu implies producer-id=%llu, outside expected [0, %u)\n",
                static_cast<unsigned long long>(msg->seq), static_cast<unsigned long long>(producerId),
                cfg.producerCount);
        }

        if (received >= cfg.warmup) {
            // idle-timeout bookkeeping above always uses clockNow() regardless of --clock; only the
            // per-message latency figure itself switches source, and only readTsc() here -- no
            // clock_gettime() call -- when --clock rdtsc is selected.
            auto const latencyNs =
                cfg.clockSource == bench::ClockSource::Tsc
                    ? tscCalibration->ticksToNs(static_cast<std::int64_t>(bench::readTsc()) - msg->sendTimeNs)
                    : nowWall - msg->sendTimeNs;
            collector.record(static_cast<std::uint64_t>(latencyNs));
        }

        consumer.consume();
        ++received;
        lastActivity = nowWall;
    }

    auto const elapsedSec = static_cast<double>(bench::clockNow() - startTime) / 1e9;

    if (bench::signalHandler().terminationRequested()) {
        std::printf("interrupted\n");
    }

    bool allValid = true;
    std::uint64_t totalMissing = 0;
    std::uint64_t totalAnomalies = 0;
    for (unsigned p = 0; p < cfg.producerCount; ++p) {
        allValid = allValid && validators[p].valid();
        totalMissing += validators[p].missing();
        totalAnomalies += validators[p].outOfOrder();
    }

    std::printf("\n--- results ---\n");
    std::printf("%-14s : %llu / %llu\n", "received", static_cast<unsigned long long>(received),
        static_cast<unsigned long long>(totalExpected));
    std::printf("%-14s : %.3f sec (%.0f msg/s)\n", "elapsed", elapsedSec,
        elapsedSec > 0 ? static_cast<double>(received) / elapsedSec : 0.0);

    std::printf("\n--- validity ---\n");
    std::printf("%-14s : %s (missing=%llu, out-of-order/duplicate=%llu)\n", "sequence check",
        allValid ? "OK" : "FAILED", static_cast<unsigned long long>(totalMissing),
        static_cast<unsigned long long>(totalAnomalies));
    if constexpr (requires { consumer.overrunCount(); }) {
        std::printf("%-14s : %llu\n", "overruns", static_cast<unsigned long long>(consumer.overrunCount()));
    }

    bench::printReport(collector.makeReport());

    return allValid && received == totalExpected;
}

auto runSPSC(Config const& cfg) -> int {
    auto memorySource = makeMemorySource(cfg);

    auto queue = cfg.openOnly
                     ? turboq::SPSCMessageQueue{cfg.queueName, memorySource}
                     : turboq::SPSCMessageQueue{cfg.queueName,
                           turboq::SPSCMessageQueue::CreationOptions{.capacityHint = cfg.capacityBytes}, memorySource};

    if (cfg.role == Role::Producer) {
        auto producer = queue.createProducer();
        runProducer(producer, cfg);
        return EXIT_SUCCESS;
    }

    auto consumer = queue.createConsumer();
    return runConsumer(consumer, cfg) ? EXIT_SUCCESS : EXIT_FAILURE;
}

auto runMPSC(Config const& cfg) -> int {
    auto memorySource = makeMemorySource(cfg);

    auto queue = cfg.openOnly ? turboq::MPSCMessageQueue{cfg.queueName, memorySource}
                              : turboq::MPSCMessageQueue{cfg.queueName,
                                    turboq::MPSCMessageQueue::CreationOptions{
                                        .slotSizeHint = cfg.messageSize, .lengthHint = cfg.lengthHint},
                                    memorySource};

    if (cfg.role == Role::Producer) {
        auto producer = queue.createProducer();
        runProducer(producer, cfg);
        return EXIT_SUCCESS;
    }

    auto consumer = queue.createConsumer();
    return runConsumer(consumer, cfg) ? EXIT_SUCCESS : EXIT_FAILURE;
}

auto runMulticast(Config const& cfg) -> int {
    auto memorySource = makeMemorySource(cfg);

    auto queue = cfg.openOnly ? turboq::MulticastMessageQueue{cfg.queueName, memorySource}
                              : turboq::MulticastMessageQueue{cfg.queueName,
                                    turboq::MulticastMessageQueue::CreationOptions{.capacityHint = cfg.capacityBytes},
                                    memorySource};

    if (cfg.role == Role::Producer) {
        auto producer = queue.createProducer();
        runProducer(producer, cfg);
        return EXIT_SUCCESS;
    }

    auto consumer = queue.createConsumer();
    return runConsumer(consumer, cfg) ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        cxxopts::Options options{
            "latency_bench", "Producer or consumer for a turboq queue (spsc/mpsc/multicast), one role per run"};
        options.set_width(120);
        options.set_tab_expansion();

        // clang-format off
        options.add_options()
            ("role", "producer or consumer", cxxopts::value<std::string>())
            ("t,type", "queue type: spsc, mpsc or multicast", cxxopts::value<std::string>()->default_value("spsc"))
            ("n,name", "queue name", cxxopts::value<std::string>()->default_value("turboq.bench"))
            ("c,capacity", "queue capacity hint in MiB (spsc/multicast); give it to create the queue if "
                "missing, omit it to open an existing queue only", cxxopts::value<std::size_t>()->default_value("64"))
            ("length", "ring length hint in slots (mpsc); give it to create the queue if missing, omit it "
                "to open an existing queue only", cxxopts::value<std::size_t>()->default_value("65536"))
            ("s,size", "message size in bytes, including the sequence/timestamp header (min 16)",
                cxxopts::value<std::size_t>()->default_value("64"))
            ("m,count", "producer: messages to send. consumer: messages to expect, per producer",
                cxxopts::value<std::uint64_t>()->default_value("1000000"))
            ("r,rate", "[producer] messages per second (0 = as fast as possible)", cxxopts::value<double>()->default_value("0"))
            ("producer-id", "[producer, mpsc] this instance's 0-based slot when running several producers concurrently",
                cxxopts::value<unsigned>()->default_value("0"))
            ("producers", "[consumer, mpsc] how many distinct --producer-id instances to expect",
                cxxopts::value<unsigned>()->default_value("1"))
            ("warmup", "[consumer] messages excluded from latency stats (still validated)",
                cxxopts::value<std::uint64_t>()->default_value("1000"))
            ("i,idle", "[consumer] max idle time with no data before giving up early (ms)",
                cxxopts::value<std::uint64_t>()->default_value("5000"))
            ("hugepages", "backing memory: auto, none, 2m or 1g. Must match between producer and "
                "consumer -- they resolve to different mount points otherwise and the consumer "
                "won't find the queue", cxxopts::value<std::string>()->default_value("none"))
            ("drain", "drain queue before start processing")
            ("clock", "per-message timestamp source: wall (clock_gettime) or rdtsc (__builtin_ia32_rdtsc, cheaper "
                "but needs a synchronized invariant TSC -- must match between producer and consumer)",
                cxxopts::value<std::string>()->default_value("wall"))
            ("h,help", "print help and exit")
        ;
        // clang-format on

        auto args = options.parse(argc, argv);
        if (args.count("help")) {
            std::fprintf(stdout, "%s\n", options.help().c_str());
            return EXIT_SUCCESS;
        }

        if (!args.count("role")) {
            std::fprintf(stderr, "ERROR: --role is required (producer or consumer)\n");
            return EXIT_FAILURE;
        }

        Config cfg;
        cfg.role = parseRole(args["role"].as<std::string>());
        cfg.type = parseQueueType(args["type"].as<std::string>());
        cfg.queueName = args["name"].as<std::string>();
        cfg.capacityBytes = args["capacity"].as<std::size_t>() * 1024ull * 1024ull;
        cfg.lengthHint = args["length"].as<std::size_t>();
        cfg.messageSize = args["size"].as<std::size_t>();
        cfg.count = args["count"].as<std::uint64_t>();
        cfg.rate = args["rate"].as<double>();
        cfg.producerId = args["producer-id"].as<unsigned>();
        cfg.producerCount = args["producers"].as<unsigned>();
        cfg.warmup = args["warmup"].as<std::uint64_t>();
        cfg.idleMs = args["idle"].as<std::uint64_t>();
        cfg.hugePages = parseHugePages(args["hugepages"].as<std::string>());
        cfg.drain = args.count("drain");
        // Open-only when no queue-sizing argument was given explicitly: the queue must already
        // exist. With size/capacity/length given, open-or-create (either role may initialize).
        cfg.openOnly = (cfg.type == QueueType::MPSC) ? !args.count("length") : !args.count("capacity");
        cfg.clockSource = parseClockSource(args["clock"].as<std::string>());

        if (cfg.messageSize < sizeof(bench::Message)) {
            std::fprintf(stderr, "ERROR: --size must be >= %zu bytes (message header size)\n", sizeof(bench::Message));
            return EXIT_FAILURE;
        }
        if (cfg.count == 0) {
            std::fprintf(stderr, "ERROR: --count must be > 0\n");
            return EXIT_FAILURE;
        }
        if (cfg.rate < 0) {
            std::fprintf(stderr, "ERROR: --rate must be >= 0\n");
            return EXIT_FAILURE;
        }
        if (cfg.producerCount == 0) {
            std::fprintf(stderr, "ERROR: --producers must be >= 1\n");
            return EXIT_FAILURE;
        }
        if (cfg.type != QueueType::MPSC && (cfg.producerId != 0 || cfg.producerCount != 1)) {
            std::fprintf(stderr, "ERROR: --producer-id / --producers are only meaningful for --type mpsc\n");
            return EXIT_FAILURE;
        }
        if (cfg.role == Role::Producer && cfg.producerId >= cfg.producerCount && cfg.producerCount != 1) {
            std::fprintf(stderr, "ERROR: --producer-id must be < --producers\n");
            return EXIT_FAILURE;
        }
        if (cfg.role == Role::Consumer && cfg.warmup >= cfg.count) {
            std::fprintf(stderr, "ERROR: --warmup must be less than --count\n");
            return EXIT_FAILURE;
        }
        if (cfg.clockSource == bench::ClockSource::Tsc && !bench::kTscSupported) {
            std::fprintf(stderr, "ERROR: --clock rdtsc is not available on this platform (x86/x86_64 only)\n");
            return EXIT_FAILURE;
        }

        switch (cfg.type) {
        case QueueType::SPSC: return runSPSC(cfg);
        case QueueType::MPSC: return runMPSC(cfg);
        case QueueType::Multicast: return runMulticast(cfg);
        }
        return EXIT_FAILURE;

    } catch (std::exception const& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return EXIT_FAILURE;
    }
}
