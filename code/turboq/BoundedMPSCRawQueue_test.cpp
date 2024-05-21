// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdint>
#include <string>

#include <doctest/doctest.h>

#include "BoundedMPSCRawQueue.h"
#include "testing.h"

namespace turboq::testing {

TEST_CASE("BoundedMPSCRawQueue: basic") {
  BoundedMPSCRawQueue queue(
      "test", BoundedMPSCRawQueue::CreationOptions(sizeof(std::uint64_t), 10), AnonymousMemorySource());

  auto producer = queue.createProducer();
  REQUIRE(producer);

  auto consumer = queue.createConsumer();
  REQUIRE(consumer);

  REQUIRE(producer.maxMessageSize() == consumer.maxMessageSize());
  REQUIRE(producer.length() == consumer.length());
  REQUIRE(producer.maxMessageSize() >= sizeof(std::uint64_t));
  REQUIRE(producer.length() >= 10);

  INFO("Requested maxMessageSize is ", sizeof(uint64_t));
  INFO("Actual maxMessageSize is ", producer.maxMessageSize());
  INFO("Requested length is ", 10);
  INFO("Actual length is ", producer.length());

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
  REQUIRE(!fetch(consumer, value));
  REQUIRE(value == std::uint64_t(-1));

  REQUIRE(!dequeue(consumer, value));
  REQUIRE(value == std::uint64_t(-1));
}

#if 0
TEST_CASE("BoundedMPSCRawQueue: multipleMessages0") {
  REQUIRE(sizeof(char) == sizeof(std::byte));

  BoundedMPSCRawQueue queue("test", BoundedMPSCRawQueue::CreationOptions(512, 1000), AnonymousMemorySource());

  auto producer = queue.createProducer();
  auto consumer = queue.createConsumer();

  REQUIRE(producer);
  REQUIRE(consumer);

  std::string data(256, 'a');

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
    consumer.consume();
    REQUIRE_EQ(str, data);
  };

  for (std::size_t i = 0; i < 1000; ++i) {
    for (std::size_t j = 0; j < 100; ++j) {
      send();
    }
    for (std::size_t j = 0; j < 100; ++j) {
      recv();
    }
  }
}
#endif

} // namespace turboq::testing
