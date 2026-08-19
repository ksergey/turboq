// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <bit>
#include <concepts>
#include <cstddef>

namespace turboq {

template <typename T>
    requires std::integral<T>
[[nodiscard]] constexpr auto upperPow2(T value) noexcept -> T {
    return std::bit_ceil<T>(value);
}

template <typename T>
    requires std::integral<T>
[[nodiscard]] constexpr auto alignUp(T value, T align) noexcept -> T {
    return ((value + align - 1) / align) * align;
}

} // namespace turboq
