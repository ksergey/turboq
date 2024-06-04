// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <algorithm>
#include <string>

#include <doctest/doctest.h>

#include "BoundedSPMCRawQueue.h"
#include "utils.h"

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

} // namespace turboq::testing
