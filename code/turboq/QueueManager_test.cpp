// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#include <cerrno>
#include <cstdint>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "QueueManager.h"
#include "TestUtils.h"

namespace turboq::testing {

TEST_SUITE("QueueManager") {

    struct Message {
        std::uint64_t seq;
    };

    // A local class cannot have a static data member, so this needs to live at namespace scope.
    struct CustomSPSCOptions {
        static constexpr std::string_view tag{"myapp/spsc"};
        static constexpr std::size_t reserveSpace{0};
    };
    using CustomSPSCMessageQueue = detail::SPSCMessageQueueImpl<CustomSPSCOptions>;

    /// Parse text expecting Error::InvalidConfig, return the exception message
    auto parseError(std::string_view text) -> std::string {
        try {
            [[maybe_unused]] auto config = QueueManager::fromString(text);
        } catch (std::system_error const& e) {
            REQUIRE_EQ(e.code(), makeErrorCode(Error::InvalidConfig));
            return e.what();
        }
        FAIL("expected Error::InvalidConfig for: " << text);
        return {};
    }

    /// Parse a single "q" queue entry and return its spec
    auto parseEntry(std::string_view entry) -> QueueSpec {
        auto const config = QueueManager::fromString(std::format(R"({{ "queues": {{ "q": {} }} }})", entry));
        REQUIRE(config.find("q"));
        return *config.find("q");
    }

    /// Error message for a single "q" queue entry
    auto entryError(std::string_view entry) -> std::string {
        return parseError(std::format(R"({{ "queues": {{ "q": {} }} }})", entry));
    }

    auto contains(std::string const& text, std::string_view part) -> bool {
        return text.find(part) != std::string::npos;
    }

    TEST_CASE("parses all queue types") {
        auto const config = QueueManager::fromString(R"(
            // comments are allowed
            {
              "queues": {
                "orders": { "type": "spsc", "capacity": "64MiB", "hugepages": "auto" },
                "events": { "type": "mpsc", "slotSize": 256, "length": 65536 },
                /* block comments too */
                "md": { "type": "multicast", "capacity": "1G", "path": "/dev/shm", "pageSize": "4K" }
              }
            }
        )");

        REQUIRE_EQ(config.specs().size(), 3);
        REQUIRE(config.contains("orders"));
        REQUIRE_FALSE(config.contains("missing"));
        REQUIRE_EQ(config.find("missing"), nullptr);

        auto const& orders = *config.find("orders");
        REQUIRE_EQ(orders.name, "orders");
        REQUIRE_EQ(orders.type, QueueType::SPSC);
        REQUIRE_EQ(orders.capacity, 64u << 20);
        REQUIRE_EQ(orders.hugePages, HugePagesOption::Auto);
        REQUIRE_FALSE(orders.path);

        auto const& events = *config.find("events");
        REQUIRE_EQ(events.type, QueueType::MPSC);
        REQUIRE_EQ(events.slotSize, 256);
        REQUIRE_EQ(events.length, 65536);
        REQUIRE_EQ(events.capacity, 0);
        REQUIRE_EQ(events.hugePages, HugePagesOption::None);

        auto const& md = *config.find("md");
        REQUIRE_EQ(md.type, QueueType::Multicast);
        REQUIRE_EQ(md.capacity, std::size_t{1} << 30);
        REQUIRE(md.path);
        REQUIRE_EQ(*md.path, "/dev/shm");
        REQUIRE_EQ(md.pageSize, 4096);
    }

    TEST_CASE("empty queue list is valid") {
        REQUIRE(QueueManager::fromString(R"({ "queues": {} })").specs().empty());
    }

    TEST_CASE("sizes") {
        auto const capacity = [](std::string_view value) {
            return parseEntry(std::format(R"({{ "type": "spsc", "capacity": {} }})", value)).capacity;
        };

        REQUIRE_EQ(capacity("4096"), 4096);
        REQUIRE_EQ(capacity(R"("4096")"), 4096);
        REQUIRE_EQ(capacity(R"("4K")"), 4096);
        REQUIRE_EQ(capacity(R"("4kib")"), 4096);
        REQUIRE_EQ(capacity(R"("2M")"), 2u << 20);
        REQUIRE_EQ(capacity(R"("2MiB")"), 2u << 20);
        REQUIRE_EQ(capacity(R"("3g")"), std::size_t{3} << 30);
        REQUIRE_EQ(capacity(R"("1GiB")"), std::size_t{1} << 30);

        for (auto const* bad :
            {"0", R"("0K")", "-1", "1.5", "true", R"("")", R"("K")", R"("4X")", R"("4 K")", R"("4KB")", R"(" 4K")"}) {
            CAPTURE(bad);
            REQUIRE(
                contains(entryError(std::format(R"({{ "type": "spsc", "capacity": {} }})", bad)), "queues.q.capacity"));
        }

        REQUIRE(contains(entryError(R"({ "type": "spsc", "capacity": "99999999999999999999" })"), "too large"));
        REQUIRE(contains(entryError(R"({ "type": "spsc", "capacity": "17179869184G" })"), "too large"));
    }

    TEST_CASE("hugepages") {
        auto const hugePages = [](std::string_view value) {
            return parseEntry(std::format(R"({{ "type": "spsc", "capacity": 4096, "hugepages": "{}" }})", value))
                .hugePages;
        };

        REQUIRE_EQ(hugePages("none"), HugePagesOption::None);
        REQUIRE_EQ(hugePages("auto"), HugePagesOption::Auto);
        REQUIRE_EQ(hugePages("2m"), HugePagesOption::HugePages2M);
        REQUIRE_EQ(hugePages("2M"), HugePagesOption::HugePages2M);
        REQUIRE_EQ(hugePages("1g"), HugePagesOption::HugePages1G);
        REQUIRE_EQ(hugePages("1G"), HugePagesOption::HugePages1G);

        REQUIRE(
            contains(entryError(R"({ "type": "spsc", "capacity": 4096, "hugepages": "4k" })"), "queues.q.hugepages"));
        REQUIRE(
            contains(entryError(R"({ "type": "spsc", "capacity": 4096, "hugepages": true })"), "queues.q.hugepages"));
    }

    TEST_CASE("validation errors") {
        // malformed JSON / top-level structure
        REQUIRE(contains(parseError("{"), "parse error"));
        REQUIRE(contains(parseError("[]"), "top level"));
        REQUIRE(contains(parseError("{}"), "\"queues\""));
        REQUIRE(contains(parseError(R"({ "queues": [] })"), "queues: expected object"));
        REQUIRE(contains(parseError(R"({ "queues": {}, "extra": 1 })"), "extra: unknown key"));

        // queue names become file names
        REQUIRE(contains(
            parseError(R"({ "queues": { "a/b": { "type": "spsc", "capacity": 4096 } } })"), "invalid queue name"));
        REQUIRE(contains(
            parseError(R"({ "queues": { "..": { "type": "spsc", "capacity": 4096 } } })"), "invalid queue name"));
        REQUIRE(contains(
            parseError(R"({ "queues": { "": { "type": "spsc", "capacity": 4096 } } })"), "invalid queue name"));

        // entry structure
        REQUIRE(contains(entryError("42"), "queues.q: expected object"));
        REQUIRE(contains(entryError(R"({ "capacity": 4096 })"), "\"type\""));
        REQUIRE(contains(entryError(R"({ "type": "fifo", "capacity": 4096 })"), "queues.q.type"));
        REQUIRE(contains(
            entryError(R"({ "type": "spsc", "capacity": 4096, "capactiy": 1 })"), "queues.q.capactiy: unknown key"));

        // per-type parameters
        REQUIRE(contains(entryError(R"({ "type": "spsc" })"), "\"capacity\" for spsc"));
        REQUIRE(contains(entryError(R"({ "type": "multicast" })"), "\"capacity\" for multicast"));
        REQUIRE(contains(entryError(R"({ "type": "mpsc", "length": 16 })"), "\"slotSize\" for mpsc"));
        REQUIRE(contains(entryError(R"({ "type": "mpsc", "slotSize": 64 })"), "\"length\" for mpsc"));
        REQUIRE(contains(entryError(R"({ "type": "mpsc", "slotSize": 64, "length": 16, "capacity": 4096 })"),
            "queues.q.capacity: not applicable to mpsc"));
        REQUIRE(contains(entryError(R"({ "type": "spsc", "capacity": 4096, "length": 16 })"),
            "queues.q.length: not applicable to spsc"));

        // memory location
        REQUIRE(contains(entryError(R"({ "type": "spsc", "capacity": 4096, "path": "/tmp", "hugepages": "auto" })"),
            "mutually exclusive"));
        REQUIRE(contains(entryError(R"({ "type": "spsc", "capacity": 4096, "pageSize": 4096 })"), "requires \"path\""));
        REQUIRE(contains(
            entryError(R"({ "type": "spsc", "capacity": 4096, "path": "/tmp", "pageSize": 3000 })"), "power of two"));
        REQUIRE(contains(entryError(R"({ "type": "spsc", "capacity": 4096, "path": "" })"), "queues.q.path"));
        REQUIRE(contains(entryError(R"({ "type": "spsc", "capacity": 4096, "path": 1 })"), "queues.q.path"));
    }

    TEST_CASE("open flags") {
        REQUIRE_EQ(QueueManager{}.openFlags(), MemorySource::OpenOnly);
        REQUIRE_EQ(QueueManager::fromString(R"({ "queues": {} })").openFlags(), MemorySource::OpenOnly);
        REQUIRE_EQ(QueueManager::fromString(R"({ "queues": {} })", kOpenOnly).openFlags(), MemorySource::OpenOnly);
        REQUIRE_EQ(
            QueueManager::fromString(R"({ "queues": {} })", kOpenOrCreate).openFlags(), MemorySource::OpenOrCreate);
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "construct from file") {
        auto const dir = makeTempMemorySource().path();
        auto const file = dir / "queues.json";
        {
            std::ofstream stream{file};
            stream << R"({ "queues": { "q": { "type": "spsc", "capacity": "1M" } } })";
        }

        auto const openOnly = QueueManager{file};
        REQUIRE(openOnly.contains("q"));
        REQUIRE_EQ(openOnly.openFlags(), MemorySource::OpenOnly);

        REQUIRE_EQ(QueueManager{file, kOpenOnly}.openFlags(), MemorySource::OpenOnly);
        REQUIRE_EQ(QueueManager{file, kOpenOrCreate}.openFlags(), MemorySource::OpenOrCreate);

        auto const made = QueueManager::makeQueueManager(file, kOpenOrCreate);
        REQUIRE(made);
        REQUIRE_EQ(made->openFlags(), MemorySource::OpenOrCreate);

        // missing file: posix error, not InvalidConfig
        auto const missing = QueueManager::makeQueueManager(dir / "missing.json");
        REQUIRE_FALSE(missing);
        REQUIRE_EQ(missing.error(), makePosixErrorCode(ENOENT));

        // errors in a file name the file
        auto const broken = dir / "broken.json";
        {
            std::ofstream stream{broken};
            stream << R"({ "queues": { "q": { "type": "spsc" } } })";
        }
        try {
            [[maybe_unused]] QueueManager unused{broken, kOpenOrCreate};
            FAIL("expected Error::InvalidConfig");
        } catch (std::system_error const& e) {
            REQUIRE_EQ(e.code(), makeErrorCode(Error::InvalidConfig));
            REQUIRE(contains(e.what(), broken.string()));
            REQUIRE(contains(e.what(), "queues.q"));
        }
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "spsc") {
        auto const text = std::format(R"({{
            "queues": {{ "orders": {{ "type": "spsc", "capacity": "1MiB", "path": "{}" }} }}
        }})",
            makeTempMemorySource().path().string());
        auto const creator = QueueManager::fromString(text, kOpenOrCreate);
        auto const opener = QueueManager::fromString(text);

        // open only: fails while the queue doesn't exist
        auto const notCreated = opener.tryGet<SPSCMessageQueue>("orders");
        REQUIRE_FALSE(notCreated);
        REQUIRE_EQ(notCreated.error(), makePosixErrorCode(ENOENT));

        auto producerQueue = creator.get<SPSCMessageQueue>("orders");
        auto consumerQueue = opener.get<SPSCMessageQueue>("orders");

        auto producer = producerQueue.createProducer();
        auto consumer = consumerQueue.createConsumer();
        REQUIRE_EQ(producer.capacity(), consumer.capacity());

        for (std::uint64_t seq = 0; seq < 1000; ++seq) {
            REQUIRE(enqueue(producer, Message{.seq = seq}));
            Message msg;
            REQUIRE(dequeue(consumer, msg));
            REQUIRE_EQ(msg.seq, seq);
        }

        // open or create on an existing queue opens it
        REQUIRE(creator.tryGet<SPSCMessageQueue>("orders"));
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "mpsc") {
        auto const text = std::format(R"({{
            "queues": {{ "events": {{ "type": "mpsc", "slotSize": 64, "length": 1024, "path": "{}" }} }}
        }})",
            makeTempMemorySource().path().string());

        REQUIRE_FALSE(QueueManager::fromString(text).tryGet<MPSCMessageQueue>("events"));

        auto producerQueue = QueueManager::fromString(text, kOpenOrCreate).get<MPSCMessageQueue>("events");
        auto consumerQueue = QueueManager::fromString(text).get<MPSCMessageQueue>("events");

        auto producer = producerQueue.createProducer();
        auto consumer = consumerQueue.createConsumer();
        REQUIRE_GE(producer.slotSize(), 64);

        for (std::uint64_t seq = 0; seq < 100; ++seq) {
            REQUIRE(enqueue(producer, Message{.seq = seq}));
        }
        for (std::uint64_t seq = 0; seq < 100; ++seq) {
            Message msg;
            REQUIRE(dequeue(consumer, msg));
            REQUIRE_EQ(msg.seq, seq);
        }
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "multicast") {
        auto const text = std::format(R"({{
            "queues": {{ "md": {{ "type": "multicast", "capacity": "1MiB", "path": "{}" }} }}
        }})",
            makeTempMemorySource().path().string());

        REQUIRE_FALSE(QueueManager::fromString(text).tryGet<MulticastMessageQueue>("md"));

        auto producerQueue = QueueManager::fromString(text, kOpenOrCreate).get<MulticastMessageQueue>("md");
        auto consumerQueue = QueueManager::fromString(text).get<MulticastMessageQueue>("md");

        auto producer = producerQueue.createProducer();
        auto consumer = consumerQueue.createConsumer();

        REQUIRE(enqueue(producer, Message{.seq = 42}));
        Message msg;
        REQUIRE(dequeue(consumer, msg));
        REQUIRE_EQ(msg.seq, 42);
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "lookup errors") {
        auto const text = std::format(R"({{
            "queues": {{ "orders": {{ "type": "spsc", "capacity": "1MiB", "path": "{}" }} }}
        }})",
            makeTempMemorySource().path().string());
        auto const manager = QueueManager::fromString(text, kOpenOrCreate);

        auto const notFound = manager.tryGet<SPSCMessageQueue>("missing");
        REQUIRE_FALSE(notFound);
        REQUIRE_EQ(notFound.error(), makeErrorCode(Error::QueueNotFound));

        auto const wrongType = manager.tryGet<MPSCMessageQueue>("orders");
        REQUIRE_FALSE(wrongType);
        REQUIRE_EQ(wrongType.error(), makeErrorCode(Error::QueueTypeMismatch));

        try {
            [[maybe_unused]] auto unused = manager.get<MulticastMessageQueue>("orders");
            FAIL("expected Error::QueueTypeMismatch");
        } catch (std::system_error const& e) {
            REQUIRE_EQ(e.code(), makeErrorCode(Error::QueueTypeMismatch));
            REQUIRE(contains(e.what(), "configured as spsc, requested multicast"));
        }

        // empty manager knows no queues
        REQUIRE_EQ(QueueManager{}.tryGet<SPSCMessageQueue>("orders").error(), makeErrorCode(Error::QueueNotFound));
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "queue with custom options") {
        auto const text = std::format(R"({{
            "queues": {{ "custom": {{ "type": "spsc", "capacity": "1MiB", "path": "{}" }} }}
        }})",
            makeTempMemorySource().path().string());

        auto queue = QueueManager::fromString(text, kOpenOrCreate).get<CustomSPSCMessageQueue>("custom");
        REQUIRE(queue);

        // the tag is still checked: the same file can't be opened as a default SPSC queue
        auto const mismatch = QueueManager::fromString(text).tryGet<SPSCMessageQueue>("custom");
        REQUIRE_FALSE(mismatch);
        REQUIRE_EQ(mismatch.error(), makeErrorCode(Error::TagMismatch));
    }
}

} // namespace turboq::testing
