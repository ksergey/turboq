// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <concepts>
#include <cstddef>
#include <span>

namespace turboq {

/// Checks T is a zero-copy producer type: prepare() hands out a raw span into the queue's shared
/// memory for the caller to write into directly, commit() publishes it. No allocation, no copy.
template <typename T>
concept ZerocopyProducer = requires(T obj, std::size_t size) {
    { obj.prepare(size) } -> std::same_as<std::span<std::byte>>;
    { obj.commit() } -> std::same_as<void>;
    { obj.commit(size) } -> std::same_as<void>;
};

/// Checks T is a zero-copy consumer type: fetch() hands out a raw span into the queue's shared
/// memory for the caller to read directly, consume() releases it. No allocation, no copy.
template <typename T>
concept ZerocopyConsumer = requires(T obj) {
    { obj.fetch() } -> std::same_as<std::span<std::byte const>>;
    { obj.consume() } -> std::same_as<void>;
    { obj.reset() } -> std::same_as<void>;
};

} // namespace turboq
