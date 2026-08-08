// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <algorithm>
#include <atomic>
#include <barrier>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <doctest/doctest.h>

#include "MPSCQueue.h"
#include "TestUtils.h"

namespace turboq::testing {

TEST_SUITE("MPSC") {

    struct alignas(64) Message {
        std::uint64_t seq;
    };

    TEST_CASE("basic") {
        auto result = MPSCQueue::makeQueue("test",
            MPSCQueue::CreationOptions{.slotSizeHint = 2 * sizeof(Message), .lengthHint = 100},
            AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        REQUIRE(queue);

        auto producer = queue.createProducer();
        REQUIRE(producer);

        auto consumer = queue.createConsumer();
        REQUIRE(consumer);

        REQUIRE_EQ(producer.slotSize(), consumer.slotSize());
        REQUIRE_EQ(producer.length(), consumer.length());

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
        auto result = MPSCQueue::makeQueue("test",
            MPSCQueue::CreationOptions{.slotSizeHint = sizeof(Message), .lengthHint = 32}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        REQUIRE(queue);

        auto producer = queue.createProducer();
        REQUIRE(producer);

        auto consumer = queue.createConsumer();
        REQUIRE(consumer);

        REQUIRE_EQ(producer.slotSize(), consumer.slotSize());
        REQUIRE_EQ(producer.length(), consumer.length());

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

    TEST_CASE("prepare() throws when message exceeds slot size") {
        auto result = MPSCQueue::makeQueue(
            "slot-overflow", MPSCQueue::CreationOptions{.slotSizeHint = 16, .lengthHint = 8}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        auto producer = queue.createProducer();
        REQUIRE(producer);

        REQUIRE_THROWS_AS((void)producer.prepare(4096), std::system_error);
    }

    TEST_CASE("a second consumer is rejected while the first is alive") {
        auto source = makeTempMemorySource();

        auto created = MPSCQueue::makeQueue(
            "single-consumer", MPSCQueue::CreationOptions{.slotSizeHint = 64, .lengthHint = 16}, source);
        REQUIRE(created);

        // open-only, via an independent file descriptor -- required for flock() contention to be real,
        // since re-locking the same fd from within one process always succeeds
        auto handleA = std::move(created).value();
        auto consumerA = handleA.createConsumer();
        REQUIRE(consumerA);

        auto opened = MPSCQueue::makeQueue("single-consumer", source);
        REQUIRE(opened);
        auto handleB = std::move(opened).value();

        REQUIRE_THROWS_AS((void)handleB.createConsumer(), std::system_error);
    }

    TEST_CASE("producer/consumer survive being moved") {
        auto result = MPSCQueue::makeQueue("move-semantics",
            MPSCQueue::CreationOptions{.slotSizeHint = sizeof(Message), .lengthHint = 16}, AnonymousMemorySource{});
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

        MPSCQueue::Producer producerC;
        MPSCQueue::Consumer consumerC;
        producerC = std::move(producerB);
        consumerC = std::move(consumerB);

        REQUIRE(enqueue(producerC, Message{.seq = 8}));
        REQUIRE(dequeue(consumerC, msg));
        REQUIRE_EQ(msg.seq, 8);
    }

    // Regression test: an earlier version of MPSCQueueConsumer's move constructor initialized
    // lastCommitState_ from other.lastMessageHeader_ instead of other.lastCommitState_ (a
    // copy-paste slip), which either fails to compile (pointer types differ) or, had the types
    // matched, would have made consume() clear the wrong slot's commit flag after a move.
    TEST_CASE("consumer keeps in-flight fetch()/consume() state across a move") {
        auto result = MPSCQueue::makeQueue("move-inflight-fetch",
            MPSCQueue::CreationOptions{.slotSizeHint = sizeof(Message), .lengthHint = 16}, AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        auto producer = queue.createProducer();
        auto consumerA = queue.createConsumer();
        REQUIRE(producer);
        REQUIRE(consumerA);

        REQUIRE(enqueue(producer, Message{.seq = 123}));

        Message msg;
        REQUIRE(fetch(consumerA, msg)); // populates lastMessageHeader_/lastCommitState_, does not release the slot
        REQUIRE_EQ(msg.seq, 123);

        // move mid-flight: consume() must still release the exact slot that was fetched before the move
        auto consumerB = std::move(consumerA);
        consumerB.consume();

        // the slot is free again: the whole ring should now be fillable without stalling
        for (std::uint64_t seq = 0; seq < consumerB.length(); ++seq) {
            REQUIRE(enqueue(producer, Message{.seq = seq}));
        }
    }

    TEST_CASE("concurrent producers never corrupt or duplicate messages") {
        constexpr unsigned kProducers = 8;
        constexpr std::uint64_t kPerProducer = 20000;
        constexpr std::uint64_t kTotal = kProducers * kPerProducer;

        auto result = MPSCQueue::makeQueue("mpsc-race",
            MPSCQueue::CreationOptions{.slotSizeHint = sizeof(std::uint64_t), .lengthHint = 1024},
            AnonymousMemorySource{});
        REQUIRE(result);

        auto queue = std::move(result).value();
        auto consumer = queue.createConsumer();
        REQUIRE(consumer);

        std::vector<MPSCQueue::Producer> producers;
        producers.reserve(kProducers);
        for (unsigned p = 0; p < kProducers; ++p) {
            auto producer = queue.createProducer();
            REQUIRE(producer);
            producers.push_back(std::move(producer));
        }

        std::barrier startGate{kProducers};
        std::vector<std::thread> threads;
        threads.reserve(kProducers);

        for (unsigned p = 0; p < kProducers; ++p) {
            threads.emplace_back([producer = std::move(producers[p]), p, &startGate]() mutable {
                startGate.arrive_and_wait();
                auto const seqBase = static_cast<std::uint64_t>(p) * kPerProducer;
                for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                    std::uint64_t const seq = seqBase + i;
                    for (;;) {
                        auto buffer = producer.prepare(sizeof(seq));
                        if (!buffer.empty()) {
                            *std::bit_cast<std::uint64_t*>(buffer.data()) = seq;
                            producer.commit();
                            break;
                        }
                        std::this_thread::yield(); // ring full, wait for the consumer to catch up
                    }
                }
            });
        }

        std::vector<bool> seen(kTotal, false);
        std::uint64_t received = 0;
        while (received < kTotal) {
            auto buffer = consumer.fetch();
            if (buffer.empty()) {
                std::this_thread::yield();
                continue;
            }
            auto const seq = *std::bit_cast<std::uint64_t const*>(buffer.data());
            REQUIRE_LT(seq, kTotal);
            REQUIRE_FALSE(seen[seq]); // no duplicate delivery, no corrupted payload
            seen[seq] = true;
            consumer.consume();
            ++received;
        }

        for (auto& t : threads) {
            t.join();
        }

        REQUIRE_EQ(received, kTotal);
        REQUIRE(std::ranges::all_of(seen, [](bool b) {
            return b;
        }));
    }
}

} // namespace turboq::testing
