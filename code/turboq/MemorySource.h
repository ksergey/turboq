// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <filesystem>
#include <string_view>
#include <tuple>

#include <turboq/File.h>
#include <turboq/Result.h>

namespace turboq {

/// Memory source interface
struct MemorySource {
  enum OpenFlags { OpenOnly, OpenOrCreate };

  virtual ~MemorySource() noexcept {}

  /// Get file descriptor for mapping and page size to roundup
  /// \param[in] name is memory source name
  [[nodiscard]] virtual Result<std::tuple<File, std::size_t>> open(
      [[maybe_unused]] std::string_view name, [[maybe_unused]] OpenFlags flags) const noexcept {
    return makePosixErrorCode(ENOSYS);
  }
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

  /// \see MemorySource::open
  [[nodiscard]] Result<std::tuple<File, std::size_t>> open(
      std::string_view name, OpenFlags flags) const noexcept override;
};

/// Anonymous memory source
struct AnonymousMemorySource final : public MemorySource {
  /// \see MemorySource::open
  [[nodiscard]] Result<std::tuple<File, std::size_t>> open(
      std::string_view name, OpenFlags flags) const noexcept override;
};

} // namespace turboq
