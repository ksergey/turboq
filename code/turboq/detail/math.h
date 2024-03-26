// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <bit>
#include <cstddef>

namespace turboq::detail {

[[nodiscard]] constexpr std::size_t upper_pow_2(std::size_t value) noexcept {
  return std::bit_ceil<std::size_t>(value);
}

[[nodiscard]] constexpr std::size_t ceil(std::size_t value, std::size_t mult) noexcept {
  return ((value + mult - 1) / mult) * mult;
}

} // namespace turboq::detail
