// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdint>
#include <string>

#include <doctest/doctest.h>

#include "BoundedMPSCRawQueue.h"
#include "utils.h"

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

} // namespace turboq::testing
