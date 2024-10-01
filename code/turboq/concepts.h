// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <concepts>
#include <cstddef>
#include <span>

namespace turboq {

/// Checks T is Producer type
template <typename T>
concept Producer = requires(T obj, std::size_t size) {
  { obj.prepare(size) } -> std::same_as<std::span<std::byte>>;
  { obj.commit() } -> std::same_as<void>;
  { obj.commit(size) } -> std::same_as<void>;
};

/// Checks T is Consumer type
template <typename T>
concept Consumer = requires(T obj) {
  { obj.fetch() } -> std::same_as<std::span<std::byte const>>;
  { obj.consume() } -> std::same_as<void>;
  { obj.reset() } -> std::same_as<void>;
};

} // namespace turboq
