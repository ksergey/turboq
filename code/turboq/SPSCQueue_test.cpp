// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <algorithm>
#include <string>

#include <doctest/doctest.h>

#include "SPSCQueue.h"
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
    using AltSPSCQueue = detail::SPSCQueueImpl<AltSPSCOptions>;

    TEST_CASE("basic") {
        auto result = SPSCQueue::makeQueue(
            "test", SPSCQueue::CreationOptions{.capacityHint = 1024 * 1024 * 1}, AnonymousMemorySource{});
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
        auto result =
            SPSCQueue::makeQueue("test", SPSCQueue::CreationOptions{.capacityHint = 2048}, AnonymousMemorySource{});
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
        auto result =
            SPSCQueue::makeQueue("capacity", SPSCQueue::CreationOptions{.capacityHint = 0}, AnonymousMemorySource{});
        REQUIRE_FALSE(result);
    }

    TEST_CASE("open-only on a queue that does not exist fails") {
        auto result = SPSCQueue::makeQueue("does-not-exist", AnonymousMemorySource{});
        REQUIRE_FALSE(result);
    }

    TEST_CASE("re-opening with a mismatched capacity fails") {
        auto source = makeTempMemorySource();

        auto created = SPSCQueue::makeQueue("size-mismatch", SPSCQueue::CreationOptions{.capacityHint = 4096}, source);
        REQUIRE(created);

        auto reopened = SPSCQueue::makeQueue("size-mismatch", SPSCQueue::CreationOptions{.capacityHint = 8192}, source);
        REQUIRE_FALSE(reopened);
    }

    TEST_CASE("opening a queue created with a different tag fails") {
        auto source = makeTempMemorySource();

        auto created = SPSCQueue::makeQueue("tag-mismatch", SPSCQueue::CreationOptions{.capacityHint = 4096}, source);
        REQUIRE(created);

        auto reopened = AltSPSCQueue::makeQueue("tag-mismatch", source);
        REQUIRE_FALSE(reopened);
    }

    TEST_CASE("makeProducer / makeConsumer create working endpoints directly") {
        auto source = makeTempMemorySource();

        auto producer =
            SPSCQueue::makeProducer("make-endpoints", SPSCQueue::CreationOptions{.capacityHint = 4096}, source);
        REQUIRE(producer);

        auto consumer = SPSCQueue::makeConsumer("make-endpoints", source);
        REQUIRE(consumer);

        REQUIRE(enqueue(producer.value(), Message{.seq = 42}));

        Message msg;
        REQUIRE(dequeue(consumer.value(), msg));
        REQUIRE_EQ(msg.seq, 42);
    }

    TEST_CASE("reset() drops any unconsumed backlog") {
        auto result =
            SPSCQueue::makeQueue("reset", SPSCQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
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
        auto result = SPSCQueue::makeQueue(
            "move-semantics", SPSCQueue::CreationOptions{.capacityHint = 4096}, AnonymousMemorySource{});
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
        SPSCQueue::Producer producerC;
        SPSCQueue::Consumer consumerC;
        producerC = std::move(producerB);
        consumerC = std::move(consumerB);
        REQUIRE(producerC);
        REQUIRE(consumerC);

        REQUIRE(enqueue(producerC, Message{.seq = 8}));
        REQUIRE(dequeue(consumerC, msg));
        REQUIRE_EQ(msg.seq, 8);
    }
}

} // namespace turboq::testing
