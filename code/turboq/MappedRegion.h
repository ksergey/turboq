// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <system_error>
#include <utility>

#include "File.h"

namespace turboq {

enum Advice { Normal, Random, Sequential };

/// Maps files in memory
class MappedRegion {
private:
    std::byte* data_{nullptr};
    std::size_t size_{0};

public:
    MappedRegion(MappedRegion const&) = delete;
    MappedRegion& operator=(MappedRegion const&) = delete;

    /// Move constructor
    MappedRegion(MappedRegion&& other) noexcept
        : data_{std::exchange(other.data_, nullptr)}, size_{std::exchange(other.size_, 0)} {}

    /// Move assignment
    MappedRegion& operator=(MappedRegion&& other) noexcept {
        if (this != &other) {
            this->~MappedRegion();
            new (this) MappedRegion{std::move(other)};
        }
        return *this;
    }

    /// Construct empty object for late initialization
    MappedRegion() = default;

    /// Construct mapped region from pointer to data and size. Own early mapped region with mmap
    MappedRegion(std::byte* data, std::size_t size) noexcept : data_{data}, size_{size} {}

    /// Construct mapped region from file descriptor, throw std::system_error on error
    explicit MappedRegion(File const& file);

    /// \overload
    MappedRegion(File const& file, std::size_t fileSize);

    /// Destructor. Unmap mmaped memory if owns it.
    virtual ~MappedRegion() noexcept;

    /// Exception safe constructors
    template <typename... Args>
    static auto makeMappedRegion(Args&&... args) noexcept -> std::expected<MappedRegion, std::error_code> {
        try {
            return {MappedRegion{std::forward<Args>(args)...}};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    /// Return true if initialized
    [[nodiscard]] explicit operator bool() const noexcept {
        return size_ > 0;
    }

    /// Return pointer to data
    [[nodiscard]] auto data() const noexcept -> std::byte const* {
        return data_;
    }

    /// \overload
    [[nodiscard]] auto data() noexcept -> std::byte* {
        return data_;
    }

    /// Return size of data.
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return size_;
    }

    /// Return mapped region content
    [[nodiscard]] auto content() const noexcept -> std::span<std::byte const> {
        return {data_, size_};
    }

    /// \overload
    [[nodiscard]] auto content() noexcept -> std::span<std::byte> {
        return {data_, size_};
    }

    /// Advise the kernel about memory access
    auto advise(Advice advice) const noexcept -> std::expected<void, std::error_code> {
        return this->advise(0, size_, advice);
    }

    /// \overload
    auto advise(
        std::size_t offset, std::size_t length, Advice advice) const noexcept -> std::expected<void, std::error_code>;
};

} // namespace turboq
