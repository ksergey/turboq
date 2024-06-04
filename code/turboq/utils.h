// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <bit>
#include <type_traits>

#include "concepts.h"
#include "platform.h"

namespace turboq {

template <typename Producer, typename Data>
  requires TurboQProducer<Producer> and std::is_trivially_copyable_v<Data>
TURBOQ_FORCE_INLINE bool enqueue(Producer& producer, Data const& data) {
  auto buffer = producer.prepare(sizeof(data));
  if (buffer.empty()) {
    return false;
  }

  *std::bit_cast<Data*>(buffer.data()) = data;
  producer.commit();

  return true;
}

template <typename Consumer, typename Data>
  requires TurboQConsumer<Consumer> and std::is_trivially_copyable_v<Data>
TURBOQ_FORCE_INLINE bool dequeue(Consumer& consumer, Data& data) {
  auto buffer = consumer.fetch();
  if (buffer.empty()) {
    return false;
  }
  data = *std::bit_cast<Data const*>(buffer.data());
  consumer.consume();
  return true;
}

template <typename Consumer, typename Data>
  requires TurboQConsumer<Consumer> and std::is_trivially_copyable_v<Data>
TURBOQ_FORCE_INLINE bool fetch(Consumer& consumer, Data& data) {
  auto buffer = consumer.fetch();
  if (buffer.empty()) {
    return false;
  }
  data = *std::bit_cast<Data const*>(buffer.data());
  return true;
}

} // namespace turboq
