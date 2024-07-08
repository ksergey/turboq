// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <cerrno>
#include <cstring>
#include <exception>
#include <system_error>

#include <turboq/detail/Expected.h>
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

/// Optional with failure reason.
template <class T = void, class E = std::error_code>
using Result = detail::Expected<T, E>;

/// Return ErrorCode with posix error
TURBOQ_FORCE_INLINE detail::Unexpected<std::error_code> makePosixErrorCode(int ec) noexcept {
  return detail::Unexpected<std::error_code>(std::error_code(ec, getPosixErrorCategory()));
}

} // namespace turboq
