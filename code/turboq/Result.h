// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <cerrno>
#include <cstring>
#include <exception>
#include <system_error>

#include <turboq/platform.h>

#if __cpp_concepts >= 202002L
#include <expected>
#elif __clang_major__ >= 17
#define turboq_save__cpp_concepts
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#define __cpp_concepts 202002L
#include <expected>
#pragma clang diagnostic ignored "-Wmacro-redefined"
#define __cpp_concepts turboq_save__cpp_concepts
#undef turboq_save__cpp_concepts
#endif

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
using Result = std::expected<T, E>;

/// Return ErrorCode with posix error
TURBOQ_FORCE_INLINE std::unexpected<std::error_code> makePosixErrorCode(int ec) noexcept {
  return std::unexpected<std::error_code>(std::error_code(ec, getPosixErrorCategory()));
}

} // namespace turboq
