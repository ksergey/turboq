// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <cerrno>
#include <cstring>
#include <exception>
#include <expected>
#include <system_error>

namespace turboq {

/// Error category for posix errors
struct PosixErrorCategory final : public std::error_category {
  constexpr PosixErrorCategory() noexcept = default;

  /// \see std::error_category
  auto name() const noexcept -> char const* override {
    return "PosixError";
  }

  /// \see std::error_category
  auto message(int error) const -> std::string override {
    return strerror(error);
  }
};

/// Return const reference to PosixErrorCategory
[[nodiscard]] constexpr auto getPosixErrorCategory() noexcept -> std::error_category const& {
  static PosixErrorCategory errorCategory;
  return errorCategory;
}

/// @see boost::outcome
/// mimic: std::expected from c++23
template <class T = void, class E = std::error_code>
using Result = std::expected<T, E>;

/// @see boost::outcome
constexpr auto success() noexcept -> Result<> {
  return {};
}

/// Return ErrorCode with posix error
inline decltype(auto) makePosixErrorCode(int ec) noexcept {
  return std::unexpected(std::error_code(ec, getPosixErrorCategory()));
}

} // namespace turboq
