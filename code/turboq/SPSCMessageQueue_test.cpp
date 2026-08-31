// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <string>

#include <doctest/doctest.h>

#include "SPSCMessageQueue.h"
#include "TestUtils.h"

namespace turboq::testing {

TEST_SUITE("SPSC") {

    struct alignas(64) Message {
        std::uint64_t seq;
    };

    // A local class cannot have a static data member, so this needs to live at namespace scope
    // rather than inline inside the "opening a queue created with a different tag fails" test case.
    struct AltSPSCOptions {
        static constexpr std::string_view tag{"turboq/spsc-alt"};
    };
    using AltSPSCMessageQueue = detail::SPSCMessageQueueImpl<AltSPSCOptions>;

    TEST_CASE("basic") {
        auto result = SPSCMessageQueue::makeQueue(
            "test", SPSCMessageQueue::CreationOptions{.capacityHint = 1024 * 1024 * 1}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        REQUIRE(queue);

        auto producer = queue.createProducer();
        REQUIRE(producer);

        auto consumer = queue.createConsumer();
        REQUIRE(consumer);

        REQUIRE_EQ(producer.capacity(), consumer.capacity());

        for (std::size_t i = 0; i < 50000; ++i) {
            for (std::uint64_t seq = 0; seq < 35; ++seq) {
                REQUIRE(enqueue(producer, Message{.seq = seq}));
            }
            Message msg;
            for (std::uint64_t seq = 0; seq < 35; ++seq) {
                REQUIRE(dequeue(consumer, msg));
                REQUIRE_EQ(msg.seq, seq);
            }
        }

        Message msg;
        REQUIRE_FALSE(dequeue(consumer, msg));
    }

    TEST_CASE("full") {
        auto result = SPSCMessageQueue::makeQueue(
            "test", SPSCMessageQueue::CreationOptions{.capacityHint = 2048}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        REQUIRE(queue);

        auto producer = queue.createProducer();
        REQUIRE(producer);

        auto consumer = queue.createConsumer();
        REQUIRE(consumer);

        REQUIRE_EQ(producer.capacity(), consumer.capacity());

        std::size_t seq = 0;
        while (enqueue(producer, Message{.seq = seq})) {
            ++seq;
        }

        REQUIRE_FALSE(enqueue(producer, Message{.seq = static_cast<std::size_t>(-1)}));

        Message msg;
        for (std::uint64_t i = 0; i < seq; ++i) {
            REQUIRE(dequeue(consumer, msg));
            REQUIRE_EQ(msg.seq, i);
        }

        REQUIRE_FALSE(dequeue(consumer, msg));
    }

    TEST_CASE("capacity 0") {
        auto result = SPSCMessageQueue::makeQueue(
            "capacity", SPSCMessageQueue::CreationOptions{.capacityHint = 0}, AnonymousMemorySource{});
        REQUIRE_FALSE(result);
    }

    TEST_CASE("open-only on a queue that does not exist fails") {
        auto result = SPSCMessageQueue::makeQueue("does-not-exist", AnonymousMemorySource{});
        REQUIRE_FALSE(result);
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "re-opening with a mismatched capacity fails") {
        auto source = makeTempMemorySource();

        auto created = SPSCMessageQueue::makeQueue(
            "size-mismatch", SPSCMessageQueue::CreationOptions{.capacityHint = 4096}, source);
        REQUIRE(created);

        auto reopened = SPSCMessageQueue::makeQueue(
            "size-mismatch", SPSCMessageQueue::CreationOptions{.capacityHint = 8192}, source);
        REQUIRE_FALSE(reopened);
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "opening a queue created with a different tag fails") {
        auto source = makeTempMemorySource();

        auto created = SPSCMessageQueue::makeQueue(
            "tag-mismatch", SPSCMessageQueue::CreationOptions{.capacityHint = 4096}, source);
        REQUIRE(created);

        auto reopened = AltSPSCMessageQueue::makeQueue("tag-mismatch", source);
        REQUIRE_FALSE(reopened);
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "makeProducer / makeConsumer create working endpoints directly") {
        auto source = makeTempMemorySource();

        auto producer = SPSCMessageQueue::makeProducer(
            "make-endpoints", SPSCMessageQueue::CreationOptions{.capacityHint = 4096}, source);
        REQUIRE(producer);

        auto consumer = SPSCMessageQueue::makeConsumer("make-endpoints", source);
        REQUIRE(consumer);

        REQUIRE(enqueue(producer.value(), Message{.seq = 42}));

        Message msg;
        REQUIRE(dequeue(consumer.value(), msg));
        REQUIRE_EQ(msg.seq, 42);
    }

    TEST_CASE("reset() drops any unconsumed backlog") {
        auto result = SPSCMessageQueue::makeQueue(
            "reset", SPSCMessageQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        auto producer = queue.createProducer();
        auto consumer = queue.createConsumer();

        for (std::uint64_t seq = 0; seq < 5; ++seq) {
            REQUIRE(enqueue(producer, Message{.seq = seq}));
        }

        consumer.reset();

        Message msg;
        REQUIRE_FALSE(dequeue(consumer, msg));

        // the queue keeps working normally after reset
        REQUIRE(enqueue(producer, Message{.seq = 100}));
        REQUIRE(dequeue(consumer, msg));
        REQUIRE_EQ(msg.seq, 100);
    }

    TEST_CASE("producer/consumer survive being moved") {
        auto result = SPSCMessageQueue::makeQueue(
            "move-semantics", SPSCMessageQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();

        auto producerA = queue.createProducer();
        auto consumerA = queue.createConsumer();
        REQUIRE(producerA);
        REQUIRE(consumerA);

        // move-construct
        auto producerB = std::move(producerA);
        auto consumerB = std::move(consumerA);
        REQUIRE_FALSE(producerA); // NOLINT(bugprone-use-after-move) -- intentionally checking moved-from state
        REQUIRE_FALSE(consumerA); // NOLINT(bugprone-use-after-move)
        REQUIRE(producerB);
        REQUIRE(consumerB);

        REQUIRE(enqueue(producerB, Message{.seq = 7}));
        Message msg;
        REQUIRE(dequeue(consumerB, msg));
        REQUIRE_EQ(msg.seq, 7);

        // move-assign into an already-initialized endpoint
        SPSCMessageQueue::Producer producerC;
        SPSCMessageQueue::Consumer consumerC;
        producerC = std::move(producerB);
        consumerC = std::move(consumerB);
        REQUIRE(producerC);
        REQUIRE(consumerC);

        REQUIRE(enqueue(producerC, Message{.seq = 8}));
        REQUIRE(dequeue(consumerC, msg));
        REQUIRE_EQ(msg.seq, 8);
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "a second producer is rejected while the first is alive") {
        auto source = makeTempMemorySource();

        auto created = SPSCMessageQueue::makeQueue(
            "single-producer", SPSCMessageQueue::CreationOptions{.capacityHint = 4096}, source);
        REQUIRE(created);

        // open-only, via an independent file descriptor -- required for the lock contention to be
        // real, since re-locking the same fd/OFD from within one process always succeeds
        auto handleA = std::move(created).value();
        auto producerA = handleA.createProducer();
        REQUIRE(producerA);

        auto opened = SPSCMessageQueue::makeQueue("single-producer", source);
        REQUIRE(opened);
        auto handleB = std::move(opened).value();

        REQUIRE_THROWS_AS((void)handleB.createProducer(), std::system_error);
    }

    TEST_CASE_FIXTURE(MemorySourceFixture, "a second consumer is rejected while the first is alive") {
        auto source = makeTempMemorySource();

        auto created = SPSCMessageQueue::makeQueue(
            "single-consumer", SPSCMessageQueue::CreationOptions{.capacityHint = 4096}, source);
        REQUIRE(created);

        auto handleA = std::move(created).value();
        auto consumerA = handleA.createConsumer();
        REQUIRE(consumerA);

        auto opened = SPSCMessageQueue::makeQueue("single-consumer", source);
        REQUIRE(opened);
        auto handleB = std::move(opened).value();

        REQUIRE_THROWS_AS((void)handleB.createConsumer(), std::system_error);
    }

    // Regression coverage for the fact that tryLockRegion() re-locking from the SAME fd/OFD
    // succeeds (it's a re-assertion of a lock this open file description already holds, not a
    // conflict) -- meaning the OFD lock alone does NOT catch calling createProducer()/
    // createConsumer() twice on the SAME handle. That gap is closed by an in-process guard flag
    // instead; this exercises it directly, on a single handle, no second fd involved at all.
    TEST_CASE("a second createProducer() on the SAME handle is rejected") {
        auto result = SPSCMessageQueue::makeQueue(
            "same-handle-producer", SPSCMessageQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
        REQUIRE(result);

        auto handle = std::move(result).value();
        auto producerA = handle.createProducer();
        REQUIRE(producerA);

        REQUIRE_THROWS_AS((void)handle.createProducer(), std::system_error);
    }

    TEST_CASE("a second createConsumer() on the SAME handle is rejected") {
        auto result = SPSCMessageQueue::makeQueue(
            "same-handle-consumer", SPSCMessageQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
        REQUIRE(result);

        auto handle = std::move(result).value();
        auto consumerA = handle.createConsumer();
        REQUIRE(consumerA);

        REQUIRE_THROWS_AS((void)handle.createConsumer(), std::system_error);
    }

    // Regression coverage for the actual point of using distinct byte-range locks instead of
    // flock(): producer and consumer must NOT contend with each other, including from a second,
    // independently-opened handle on the same queue -- a rejected producer (because one already
    // exists) must not prevent the SAME second handle from successfully taking the consumer slot.
    TEST_CASE_FIXTURE(MemorySourceFixture, "producer and consumer locks are independent, even across handles") {
        auto source = makeTempMemorySource();

        auto created = SPSCMessageQueue::makeQueue(
            "independent-locks", SPSCMessageQueue::CreationOptions{.capacityHint = 4096}, source);
        REQUIRE(created);

        auto handleA = std::move(created).value();
        auto producerA = handleA.createProducer();
        REQUIRE(producerA);

        auto opened = SPSCMessageQueue::makeQueue("independent-locks", source);
        REQUIRE(opened);
        auto handleB = std::move(opened).value();

        // handleB can't take the producer slot (handleA already holds it)...
        REQUIRE_THROWS_AS((void)handleB.createProducer(), std::system_error);
        // ...but the consumer slot is a completely different lock, unaffected by that rejection
        auto consumerB = handleB.createConsumer();
        REQUIRE(consumerB);

        REQUIRE(enqueue(producerA, Message{.seq = 42}));
        Message msg;
        REQUIRE(dequeue(consumerB, msg));
        REQUIRE_EQ(msg.seq, 42);
    }
}

// Exercises File::tryLockRegion()'s new `size` parameter directly (not through SPSCMessageQueue,
// which only ever locks single bytes) -- a multi-byte region must behave like a single logical
// lock: an overlapping request from another file description is rejected, a non-overlapping one
// is unaffected, and unlocking requires the same [offset, offset+size) that was locked.
namespace {
TEST_SUITE("File region locks") {
    TEST_CASE("locking [0,4) then [4,8) on the same fd does not self-conflict") {
        auto tmp = turboq::File::temporary();
        REQUIRE(tmp);
        auto file = std::move(tmp).value();
        REQUIRE(file.tryTruncate(64));

        REQUIRE(file.tryLockRegion(0, 4));
        REQUIRE(file.tryLockRegion(4, 4)); // adjacent, non-overlapping -- must not conflict with the first
    }

    TEST_CASE("a request overlapping an existing multi-byte lock is rejected") {
        auto path = std::filesystem::temp_directory_path() / "turboq_region_lock_test";
        std::filesystem::remove(path);

        auto createdA = turboq::File::makeFile(turboq::kCreateOnly, path, turboq::OpenMode::ReadWrite);
        REQUIRE(createdA);
        auto fileA = std::move(createdA).value();
        REQUIRE(fileA.tryTruncate(64));
        REQUIRE(fileA.tryLockRegion(0, 8)); // locks bytes [0, 8)

        auto createdB = turboq::File::makeFile(turboq::kOpenOnly, path, turboq::OpenMode::ReadWrite);
        REQUIRE(createdB);
        auto fileB = std::move(createdB).value();

        REQUIRE_FALSE(fileB.tryLockRegion(4, 8)); // [4, 12) overlaps [0, 8) -- must be rejected
        REQUIRE(fileB.tryLockRegion(8, 8));       // [8, 16) does not overlap -- must succeed

        std::filesystem::remove(path);
    }

    TEST_CASE("unlockRegion releases exactly the locked range, freeing it for others") {
        auto path = std::filesystem::temp_directory_path() / "turboq_region_unlock_test";
        std::filesystem::remove(path);

        auto createdA = turboq::File::makeFile(turboq::kCreateOnly, path, turboq::OpenMode::ReadWrite);
        REQUIRE(createdA);
        auto fileA = std::move(createdA).value();
        REQUIRE(fileA.tryTruncate(64));
        fileA.lockRegion(0, 16);

        auto createdB = turboq::File::makeFile(turboq::kOpenOnly, path, turboq::OpenMode::ReadWrite);
        REQUIRE(createdB);
        auto fileB = std::move(createdB).value();
        REQUIRE_FALSE(fileB.tryLockRegion(0, 16));

        fileA.unlockRegion(0, 16);
        REQUIRE(fileB.tryLockRegion(0, 16));

        std::filesystem::remove(path);
    }
}
} // namespace

} // namespace turboq::testing
