// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <algorithm>
#include <string>

#include <doctest/doctest.h>

#include "MulticastQueue.h"
#include "TestUtils.h"

namespace turboq::testing {

TEST_SUITE("MulticastQueue") {

    struct Message {
        std::uint64_t seq;
    };

    TEST_CASE("basic") {
        auto result = MulticastQueue::makeMulticastQueue(
            "test", MulticastQueue::CreationOptions{.capacityHint = 1024 * 1024 * 8}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        REQUIRE(queue);

        auto producer = queue.createProducer();
        REQUIRE(producer);

        auto consumer = queue.createConsumer();
        REQUIRE(consumer);

        REQUIRE(producer.capacity() == consumer.capacity());

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
        REQUIRE(!dequeue(consumer, msg));
    }
}

} // namespace turboq::testing
