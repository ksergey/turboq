// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <algorithm>
#include <string>

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
}

} // namespace turboq::testing
