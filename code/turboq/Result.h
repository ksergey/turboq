// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <cerrno>
#include <cstring>
#include <exception>
#include <expected>
#include <system_error>

#include <turboq/platform.h>

namespace turboq {

/// Error category for posix errors
struct PosixErrorCategory final : public std::error_category {
  constexpr PosixErrorCategory() noexcept = default;

  /// \see std::error_category
  char const* name() const noexcept override {
    return "PosixError";
  }

  /// \see std::error_category
  std::string message(int error) const override {
    return strerror(error);
  }
};

/// Return const reference to PosixErrorCategory
TURBOQ_FORCE_INLINE std::error_category const& getPosixErrorCategory() noexcept {
  static PosixErrorCategory errorCategory;
  return errorCategory;
}

/// @see boost::outcome
/// mimic: std::expected from c++23
template <class T = void, class E = std::error_code>
using Result = std::expected<T, E>;

/// @see boost::outcome
constexpr Result<> success() noexcept {
  return {};
}

/// Return ErrorCode with posix error
TURBOQ_FORCE_INLINE decltype(auto) makePosixErrorCode(int ec) noexcept {
  return std::unexpected(std::error_code(ec, getPosixErrorCategory()));
}

} // namespace turboq
