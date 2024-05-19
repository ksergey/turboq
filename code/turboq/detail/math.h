// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <bit>
#include <concepts>
#include <cstddef>

namespace turboq::detail {

template <typename T>
  requires std::integral<T>
[[nodiscard]] constexpr T upper_pow_2(T value) noexcept {
  return std::bit_ceil<T>(value);
}

template <typename T>
  requires std::integral<T>
[[nodiscard]] constexpr T align_up(T value, T align) noexcept {
  return ((value + align - 1) / align) * align;
}

} // namespace turboq::detail
