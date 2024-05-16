// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <string>

#include <doctest/doctest.h>

#include "BoundedSPSCRawQueue.h"

#include "concepts.h"

static auto const options = turboq::BoundedSPSCRawQueue::CreationOptions(1024 * 1024);

TEST_CASE("BoundedSPSCRawQueue: multipleMessages0") {
  REQUIRE(sizeof(char) == sizeof(std::byte));

  turboq::BoundedSPSCRawQueue queue("test", options, turboq::AnonymousMemorySource());

  auto producer = queue.createProducer();
  static_assert(turboq::TurboQProducer<decltype(producer)>);

  auto consumer = queue.createConsumer();
  static_assert(turboq::TurboQConsumer<decltype(consumer)>);

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
    consumer.consume();
  };

  for (std::size_t i = 0; i < 10000; ++i) {
    send();
    recv();
  }
}
