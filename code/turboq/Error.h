// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <cerrno>
#include <cstring>
#include <system_error>

namespace turboq {

/// Error category for posix errors
struct PosixErrorCategory final : public std::error_category {
    constexpr PosixErrorCategory() noexcept = default;

    /// \see std::error_category
    [[nodiscard]] auto name() const noexcept -> char const* override {
        return "PosixError";
    }

    /// \see std::error_category
    [[nodiscard]] auto message(int error) const -> std::string override {
        return ::strerror(error);
    }
};

/// Return const reference to PosixErrorCategory
[[nodiscard]] constexpr auto getPosixErrorCategory() noexcept -> std::error_category const& {
    static PosixErrorCategory errorCategory;
    return errorCategory;
}

/// Return ErrorCode with posix error
[[nodiscard]] inline auto makePosixErrorCode(int ec) noexcept -> std::error_code {
    return {ec, getPosixErrorCategory()};
}

enum class Error {
    BufferTooSmall,
    SizeMismatch,
    TagMismatch,
    InvalidCreationOptions,
    MessageSizeExceedSlotSize,
    ProducerAlreadyExists,
    ConsumerAlreadyExists,
};

struct ErrorCategory final : public std::error_category {
    constexpr ErrorCategory() = default;

    [[nodiscard]] auto name() const noexcept -> char const* override {
        return "Error";
    }

    [[nodiscard]] auto message(int error) const -> std::string override {
        switch (static_cast<Error>(error)) {
        case Error::BufferTooSmall: return "buffer size is too small for queue";
        case Error::SizeMismatch: return "queue size mismatch previously created";
        case Error::TagMismatch: return "queue tag mismatch previously created";
        case Error::InvalidCreationOptions: return "invalid creation options";
        case Error::MessageSizeExceedSlotSize: return "message size exceed slot size";
        case Error::ProducerAlreadyExists: return "queue producer already exists";
        case Error::ConsumerAlreadyExists: return "queue consumer already exists";
        default: return "?";
        }
    }
};

[[nodiscard]] constexpr auto getErrorCategory() noexcept -> std::error_category const& {
    static ErrorCategory errorCategory;
    return errorCategory;
}

[[nodiscard]] inline auto makeErrorCode(Error e) noexcept -> std::error_code {
    return {static_cast<int>(e), getErrorCategory()};
}

} // namespace turboq
