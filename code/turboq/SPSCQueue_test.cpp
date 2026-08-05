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

#if 0
    TEST_CASE("basic") {
        auto result = SPSCQueue::makeSPSCQueue(
            "test", SPSCQueue::CreationOptions{.capacityHint = 1024 * 1024 * 8}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        REQUIRE(queue);

        auto producer = queue.createProducer();
        REQUIRE(producer);

        auto consumer = queue.createConsumer();
        REQUIRE(consumer);

        REQUIRE_EQ(producer.capacity(), consumer.capacity());

        for (std::size_t i = 0; i < 500000; ++i) {
            for (std::uint64_t seq = 0; seq < 32; ++seq) {
                REQUIRE(enqueue(producer, Message{.seq = seq}));
            }
            Message msg;
            for (std::uint64_t seq = 0; seq < 32; ++seq) {
                REQUIRE(dequeue(consumer, msg));
                REQUIRE_EQ(msg.seq, seq);
            }
        }

        Message msg;
        REQUIRE_FALSE(dequeue(consumer, msg));
    }
#endif

    TEST_CASE("full") {
        auto result =
            SPSCQueue::makeSPSCQueue("test", SPSCQueue::CreationOptions{.capacityHint = 2048}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        REQUIRE(queue);

        auto producer = queue.createProducer();
        REQUIRE(producer);

        auto consumer = queue.createConsumer();
        REQUIRE(consumer);

        REQUIRE_EQ(producer.capacity(), consumer.capacity());

        std::size_t seq = 0;
        while (enqueue(producer, Message{.seq = seq++})) {
            std::printf("prod: seq + 1 = %llu\n", static_cast<unsigned long long>(seq));
        }

        REQUIRE_FALSE(enqueue(producer, Message{.seq = static_cast<std::size_t>(-1)}));

        INFO("got sequence ", seq);

        Message msg;
        for (std::uint64_t i = 0; i < seq; ++i) {
            std::printf("i = %llu\n", static_cast<unsigned long long>(i));

            REQUIRE(dequeue(consumer, msg));
            INFO("dequeued ", msg.seq, " expected ", i);
            REQUIRE_EQ(msg.seq, i);
        }

        REQUIRE_FALSE(dequeue(consumer, msg));
    }

#if 0
    TEST_CASE("capacity 0") {
        auto result = SPSCQueue::makeSPSCQueue(
            "capacity", SPSCQueue::CreationOptions{.capacityHint = 0}, AnonymousMemorySource{});
        REQUIRE_FALSE(result);
    }
#endif
}

} // namespace turboq::testing
