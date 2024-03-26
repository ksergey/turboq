// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cerrno>
#include <cstring>
#include <exception>
#include <system_error>

#include <boost/outcome.hpp>

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

/// Return ErrorCode with posix error
TURBOQ_FORCE_INLINE std::error_code makePosixErrorCode(int ec) noexcept {
  return std::error_code(ec, getPosixErrorCategory());
}

/// Optional with failure reason.
template <class T = void, class E = std::error_code>
using Result = BOOST_OUTCOME_V2_NAMESPACE::std_result<T, E>;

/// Helper to return success result
using BOOST_OUTCOME_V2_NAMESPACE::success;

/// Helper to return failure result
using BOOST_OUTCOME_V2_NAMESPACE::failure;

} // namespace turboq
