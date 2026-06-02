// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <bit>
#include <concepts>
#include <cstddef>

namespace turboq::detail {

template <typename T>
    requires std::integral<T>
[[nodiscard]] constexpr auto upper_pow_2(T value) noexcept -> T {
    return std::bit_ceil<T>(value);
}

template <typename T>
    requires std::integral<T>
[[nodiscard]] constexpr auto align_up(T value, T align) noexcept -> T {
    return ((value + align - 1) / align) * align;
}

} // namespace turboq::detail
