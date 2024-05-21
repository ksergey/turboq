// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <string>

#include <doctest/doctest.h>

#include "BoundedSPMCRawQueue.h"
#include "testing.h"

namespace turboq::testing {

TEST_CASE("BoundedSPMCRawQueue: basic") {
  BoundedSPMCRawQueue queue(
      "test", BoundedSPMCRawQueue::CreationOptions(sizeof(std::uint64_t) * 100), AnonymousMemorySource());

  auto producer = queue.createProducer();
  REQUIRE(producer);

  auto consumer = queue.createConsumer();
  REQUIRE(consumer);

  REQUIRE(producer.capacity() == consumer.capacity());

  for (std::uint64_t i = 0; i < 10; ++i) {
    REQUIRE(enqueue(producer, i));
  }

  for (std::uint64_t i = 0; i < 10; ++i) {
    std::uint64_t value = std::uint64_t(-1);

    REQUIRE(fetch(consumer, value));
    REQUIRE(value == i);

    value = std::uint64_t(-1);
    REQUIRE(fetch(consumer, value));
    REQUIRE(value == i);

    value = std::uint64_t(-1);
    REQUIRE(dequeue(consumer, value));
    REQUIRE(value == i);
  }

  std::uint64_t value = std::uint64_t(-1);
  REQUIRE(!dequeue(consumer, value));
  REQUIRE(value == std::uint64_t(-1));
}

#if 0
TEST_CASE("BoundedSPMCRawQueue: multipleMessages0") {
  REQUIRE(sizeof(char) == sizeof(std::byte));

  BoundedSPMCRawQueue queue("test", BoundedSPMCRawQueue::CreationOptions(1024 * 1024), AnonymousMemorySource());

  auto producer = queue.createProducer();
  auto consumer = queue.createConsumer();

  REQUIRE(producer);
  REQUIRE(consumer);

  std::string data(512, 'a');

  auto send = [&producer, &data] {
    auto buffer = producer.prepare(data.size());
    REQUIRE(!buffer.empty());
    std::copy(data.begin(), data.end(), std::bit_cast<char*>(buffer.data()));
    producer.commit();
  };

  auto recv = [&consumer, &data] {
    auto buffer = consumer.fetch();
    REQUIRE(!buffer.empty());
    std::string str(std::bit_cast<char const*>(buffer.data()), buffer.size());
    REQUIRE_EQ(str, data);
  };

  for (std::size_t i = 0; i < 10000; ++i) {
    send();
    recv();
  }
}
#endif

} // namespace turboq::testing
