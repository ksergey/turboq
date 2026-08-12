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
            REQUIRE_EQ(consumer.overrunCount(), 0u);
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

    TEST_CASE("consumer detects being lapped by a fast producer (overrun)") {
        // small capacity relative to the message size so the producer wraps around many times
        // over a paused consumer's position
        auto result = MulticastQueue::makeQueue(
            "overrun", MulticastQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        auto producer = queue.createProducer();
        auto consumer = queue.createConsumer();
        REQUIRE(producer);
        REQUIRE(consumer);

        // one full round-trip establishes a real sequence baseline in the consumer
        REQUIRE(enqueue(producer, Message{.seq = 0}));
        Message msg;
        REQUIRE(dequeue(consumer, msg));
        REQUIRE_EQ(msg.seq, 0);
        REQUIRE_EQ(consumer.overrunCount(), 0u);

        // let the producer lap the still-parked consumer many times over without it reading anything
        for (std::uint64_t seq = 1; seq <= 500; ++seq) {
            REQUIRE(enqueue(producer, Message{.seq = seq}));
        }

        // the slot the consumer is still parked at has long since been overwritten -- the very next
        // fetch() must report Overrun rather than hand back a stale/out-of-context message
        auto staleResult = consumer.fetch();
        REQUIRE_FALSE(staleResult);
        REQUIRE_EQ(staleResult.error(), FetchError::Overrun);
        REQUIRE_EQ(consumer.overrunCount(), 1u);

        // and it recovers: the queue is fully usable again from here, exactly like a fresh subscriber
        REQUIRE(enqueue(producer, Message{.seq = 999}));
        REQUIRE(dequeue(consumer, msg));
        REQUIRE_EQ(msg.seq, 999);
        REQUIRE_EQ(consumer.overrunCount(), 1u); // unchanged: no further overrun on the recovered stream
    }

    TEST_CASE("a fully-consumed stream across many wraps never reports a false overrun") {
        auto result = MulticastQueue::makeQueue(
            "no-false-overrun", MulticastQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        auto producer = queue.createProducer();
        auto consumer = queue.createConsumer();
        REQUIRE(producer);
        REQUIRE(consumer);

        // 10000 messages through a ring that only fits ~32 at a time -- hundreds of wraps, all
        // fully drained between writes, must never be mistaken for an overrun
        for (std::uint64_t i = 0; i < 10000; ++i) {
            REQUIRE(enqueue(producer, Message{.seq = i}));
            Message msg;
            REQUIRE(dequeue(consumer, msg));
            REQUIRE_EQ(msg.seq, i);
        }

        REQUIRE_EQ(consumer.overrunCount(), 0u);
    }

    TEST_CASE("reset() after an overrun does not resurrect the stale sequence baseline") {
        auto result = MulticastQueue::makeQueue(
            "overrun-then-reset", MulticastQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        auto producer = queue.createProducer();
        auto consumer = queue.createConsumer();
        REQUIRE(producer);
        REQUIRE(consumer);

        REQUIRE(enqueue(producer, Message{.seq = 0}));
        Message msg;
        REQUIRE(dequeue(consumer, msg));

        for (std::uint64_t seq = 1; seq <= 500; ++seq) {
            REQUIRE(enqueue(producer, Message{.seq = seq}));
        }

        consumer.reset();

        // reset() re-baselines like a fresh subscriber -- the first message seen after it must not
        // be compared against whatever sequence happened to be expected before the reset
        REQUIRE(enqueue(producer, Message{.seq = 501}));
        REQUIRE(dequeue(consumer, msg));
        REQUIRE_EQ(msg.seq, 501);
        REQUIRE_EQ(consumer.overrunCount(), 0u);
    }
}

} // namespace turboq::testing
