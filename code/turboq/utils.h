// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <bit>
#include <type_traits>

#include "concepts.h"

namespace turboq {

template <typename ProducerT, typename DataT>
  requires Producer<ProducerT> and std::is_trivially_copyable_v<DataT>
[[nodiscard]] auto enqueue(ProducerT& producer, DataT const& data) -> bool {
  auto buffer = producer.prepare(sizeof(data));
  if (buffer.empty()) {
    return false;
  }
  *std::bit_cast<DataT*>(buffer.data()) = data;
  producer.commit();
  return true;
}

template <typename ConsumerT, typename DataT>
  requires Consumer<ConsumerT> and std::is_trivially_copyable_v<DataT>
[[nodiscard]] auto dequeue(ConsumerT& consumer, DataT& data) -> bool {
  auto buffer = consumer.fetch();
  if (buffer.empty()) {
    return false;
  }
  data = *std::bit_cast<DataT const*>(buffer.data());
  consumer.consume();
  return true;
}

template <typename ConsumerT, typename DataT>
  requires Consumer<ConsumerT> and std::is_trivially_copyable_v<DataT>
[[nodiscard]] auto fetch(ConsumerT& consumer, DataT& data) -> bool {
  auto buffer = consumer.fetch();
  if (buffer.empty()) {
    return false;
  }
  data = *std::bit_cast<DataT const*>(buffer.data());
  return true;
}

} // namespace turboq
