// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <concepts>
#include <cstddef>

namespace turboq {

template <typename T>
    requires std::integral<T>
[[nodiscard]] constexpr auto align_up(T value, T align) noexcept -> T {
    return ((value + align - 1) / align) * align;
}

} // namespace turboq
