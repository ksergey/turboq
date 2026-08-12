// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <cstddef>
#include <span>

namespace turboq {

enum class FetchError : std::ptrdiff_t {
    Overrun = -1, // this consumer was lapped by the producer: one or more messages between the
                  // last successful fetch() and this one were overwritten and cannot be recovered
};

/// Result of Consumer::fetch(). A minimal, sentinel-encoded alternative to std::expected<std::span<std::byte const>,
/// FetchError>
class [[nodiscard]] FetchResult {
private:
    std::byte const* data_{nullptr};
    std::ptrdiff_t size_{0};

public:
    constexpr FetchResult() = default;

    constexpr FetchResult(std::span<std::byte const> buffer) noexcept
        : data_{buffer.data()}, size_{static_cast<std::ptrdiff_t>(buffer.size())} {}

    constexpr explicit FetchResult(FetchError ec) noexcept : size_{static_cast<std::ptrdiff_t>(ec)} {}

    /// True unless this is an error result. NOTE: this does NOT mean "has data" -- an empty,
    /// non-error result (no data yet) is also true. Use empty() to check for that separately,
    /// e.g. `if (auto r = consumer.fetch()) { if (!r.empty()) { use(*r); consumer.consume(); } }`.
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return size_ >= 0;
    }

    /// True when there is nothing to process right now: either genuinely no data yet, or (only
    /// possible in the error case) an error result, which never carries usable data either.
    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return size_ <= 0;
    }

    /// pre: operator bool() == true
    [[nodiscard]] constexpr auto value() const noexcept -> std::span<std::byte const> {
        return {data_, static_cast<std::size_t>(size_)};
    }

    /// pre: operator bool() == false
    [[nodiscard]] constexpr auto error() const noexcept -> FetchError {
        return static_cast<FetchError>(size_);
    }
};

} // namespace turboq
