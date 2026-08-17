// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <tuple>

#include "File.h"

namespace turboq {

/// Memory source interface
struct MemorySource {
    enum OpenFlags { OpenOnly, OpenOrCreate };

    virtual ~MemorySource() noexcept {}

    /// Get file descriptor for mapping and page size to round up
    /// \param[in] name is memory source name
    [[nodiscard]] virtual auto open(std::string_view name, OpenFlags flags) const noexcept
        -> std::expected<std::tuple<File, std::size_t>, std::error_code> = 0;
};

/// HugePages option selector
enum class HugePagesOption { Auto, HugePages2M, HugePages1G, None };

/// Default memory source
class DefaultMemorySource final : public MemorySource {
private:
    std::filesystem::path path_;
    std::size_t pageSize_ = 0;

public:
    /// Construct memory source
    /// \param[in] hugePagesOpt is huge pages option
    /// Throws on error
    explicit DefaultMemorySource(HugePagesOption hugePagesOpt = HugePagesOption::None);

    /// Construct memory source explicit
    /// Throws on error
    DefaultMemorySource(std::filesystem::path const& path, std::size_t pageSize);

    /// Path where shared files will be created
    [[nodiscard]] auto path() const noexcept -> std::filesystem::path const& {
        return path_;
    }

    /// \see MemorySource::open
    [[nodiscard]] auto open(std::string_view name, OpenFlags flags) const noexcept
        -> std::expected<std::tuple<File, std::size_t>, std::error_code> override;
};

/// Anonymous memory source
struct AnonymousMemorySource final : public MemorySource {
    /// \see MemorySource::open
    [[nodiscard]] auto open(std::string_view name, OpenFlags flags) const noexcept
        -> std::expected<std::tuple<File, std::size_t>, std::error_code> override;
};

} // namespace turboq
