// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <algorithm>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "MulticastQueue.h"
#include "TestUtils.h"

namespace turboq::testing {

TEST_SUITE("MulticastQueue") {

    struct alignas(64) Message {
        std::uint64_t seq;
    };

    TEST_CASE("basic") {
        auto result = MulticastQueue::makeQueue(
            "test", MulticastQueue::CreationOptions{.capacityHint = 1024 * 1024 * 1}, AnonymousMemorySource{});
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

    TEST_CASE("capacity 0") {
        auto result = MulticastQueue::makeQueue(
            "capacity", MulticastQueue::CreationOptions{.capacityHint = 0}, AnonymousMemorySource{});
        REQUIRE_FALSE(result);
    }

    TEST_CASE("a single message fans out to every independent consumer") {
        auto result = MulticastQueue::makeQueue(
            "fan-out", MulticastQueue::CreationOptions{.capacityHint = 1024 * 1024}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        auto producer = queue.createProducer();
        REQUIRE(producer);

        // consumers only see messages published after they were created, so create them up front
        constexpr int kConsumerCount = 5;
        std::vector<MulticastQueue::Consumer> consumers;
        consumers.reserve(kConsumerCount);
        for (int i = 0; i < kConsumerCount; ++i) {
            auto consumer = queue.createConsumer();
            REQUIRE(consumer);
            consumers.push_back(std::move(consumer));
        }

        constexpr std::uint64_t kMessageCount = 1000;
        for (std::uint64_t seq = 0; seq < kMessageCount; ++seq) {
            REQUIRE(enqueue(producer, Message{.seq = seq}));
        }

        // every consumer independently observes the full, identically-ordered stream
        for (auto& consumer : consumers) {
            Message msg;
            for (std::uint64_t seq = 0; seq < kMessageCount; ++seq) {
                REQUIRE(dequeue(consumer, msg));
                REQUIRE_EQ(msg.seq, seq);
            }
            REQUIRE_FALSE(dequeue(consumer, msg));
        }
    }

    TEST_CASE("a consumer created after publishing does not see the backlog") {
        auto result = MulticastQueue::makeQueue(
            "late-subscriber", MulticastQueue::CreationOptions{.capacityHint = 1024 * 1024}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        auto producer = queue.createProducer();
        REQUIRE(producer);

        for (std::uint64_t seq = 0; seq < 100; ++seq) {
            REQUIRE(enqueue(producer, Message{.seq = seq}));
        }

        auto lateConsumer = queue.createConsumer();
        REQUIRE(lateConsumer);

        Message msg;
        REQUIRE_FALSE(dequeue(lateConsumer, msg));

        REQUIRE(enqueue(producer, Message{.seq = 100}));
        REQUIRE(dequeue(lateConsumer, msg));
        REQUIRE_EQ(msg.seq, 100);
    }

    TEST_CASE("a second producer is rejected while the first is alive") {
        auto source = makeTempMemorySource();

        auto created =
            MulticastQueue::makeQueue("single-producer", MulticastQueue::CreationOptions{.capacityHint = 4096}, source);
        REQUIRE(created);

        // open-only, via an independent file descriptor -- required for flock() contention to be real,
        // since re-locking the same fd from within one process always succeeds
        auto handleA = std::move(created).value();
        auto producerA = handleA.createProducer();
        REQUIRE(producerA);

        auto opened = MulticastQueue::makeQueue("single-producer", source);
        REQUIRE(opened);
        auto handleB = std::move(opened).value();

        REQUIRE_THROWS_AS((void)handleB.createProducer(), std::system_error);
    }

    TEST_CASE("reset() fast-forwards a consumer to the current head") {
        auto result = MulticastQueue::makeQueue(
            "reset", MulticastQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
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

        REQUIRE(enqueue(producer, Message{.seq = 100}));
        REQUIRE(dequeue(consumer, msg));
        REQUIRE_EQ(msg.seq, 100);
    }

    TEST_CASE("producer/consumer survive being moved") {
        auto result = MulticastQueue::makeQueue(
            "move-semantics", MulticastQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        auto producerA = queue.createProducer();
        auto consumerA = queue.createConsumer();
        REQUIRE(producerA);
        REQUIRE(consumerA);

        auto producerB = std::move(producerA);
        auto consumerB = std::move(consumerA);
        REQUIRE_FALSE(producerA); // NOLINT(bugprone-use-after-move) -- intentionally checking moved-from state
        REQUIRE_FALSE(consumerA); // NOLINT(bugprone-use-after-move)

        REQUIRE(enqueue(producerB, Message{.seq = 7}));
        Message msg;
        REQUIRE(dequeue(consumerB, msg));
        REQUIRE_EQ(msg.seq, 7);

        MulticastQueue::Producer producerC;
        MulticastQueue::Consumer consumerC;
        producerC = std::move(producerB);
        consumerC = std::move(consumerB);

        REQUIRE(enqueue(producerC, Message{.seq = 8}));
        REQUIRE(dequeue(consumerC, msg));
        REQUIRE_EQ(msg.seq, 8);
    }
}

} // namespace turboq::testing
