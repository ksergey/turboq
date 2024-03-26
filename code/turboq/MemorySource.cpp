// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MemorySource.h"

#include <mntent.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <ranges>
#include <regex>
#include <string_view>
#include <system_error>
#include <vector>

#include <boost/scope_exit.hpp>
#include <fmt/format.h>

namespace turboq {
namespace {

std::size_t const gDefaultPageSize = ::sysconf(_SC_PAGESIZE);
constexpr std::size_t gPageSize2M = 2 * 1024 * 1024;
constexpr std::size_t gPageSize1G = 1 * 1024 * 1024 * 1024;

Result<std::size_t> getDefaultHugePageSize() noexcept {
  using namespace std::string_view_literals;

  static auto const regex = std::regex(R"!(Hugepagesize:\s*(\d+)\s*kB)!");

  auto handle = ::fopen("/proc/meminfo", "r");
  if (!handle) {
    return makePosixErrorCode(errno);
  }

  char* line = nullptr;
  std::size_t len = 0;

  BOOST_SCOPE_EXIT_ALL(&) {
    ::fclose(handle);
    if (line) {
      ::free(line);
    }
  };

  std::cmatch match;

  while (::getline(&line, &len, handle) != -1) {
    auto input = std::string_view(line, ::strlen(line) - 1);

    if (!std::regex_match(input.begin(), input.end(), match, regex)) {
      continue;
    }

    std::size_t pageSizeKiB;
    auto const rc = std::from_chars(match[1].first, match[1].second, pageSizeKiB);
    if (rc.ec == std::errc()) {
      return pageSizeKiB * 1024;
    } else {
      return makePosixErrorCode(EINVAL);
    }
  }

  return makePosixErrorCode(ENOENT);
}

Result<std::size_t> getPageSizeFromMountOpts(std::string_view opts) noexcept {
  using namespace std::string_view_literals;

  for (auto const word : std::views::split(std::string_view(opts), ","sv)) {
    auto const option = std::string_view(std::ranges::data(word), std::ranges::size(word));
    if (!option.starts_with("pagesize="sv)) {
      continue;
    }
    std::string_view value = option.substr("pagesize="sv.size());
    if (value == "2M"sv) {
      return gPageSize2M;
    } else if (value == "1G"sv) {
      return gPageSize1G;
    } else {
      return makePosixErrorCode(EINVAL);
    }
  }

  return makePosixErrorCode(ENOENT);
}

struct MemoryMountPoint {
  std::filesystem::path path;
  std::size_t pageSize;
};

std::vector<MemoryMountPoint> readProcMounts() {
  using namespace std::string_view_literals;

  auto handle = ::setmntent("/proc/mounts", "r");
  if (!handle) {
    throw std::system_error(ENOENT, getPosixErrorCategory(), "setmntent(...)");
  }

  BOOST_SCOPE_EXIT_ALL(&) {
    ::endmntent(handle);
  };

  std::vector<MemoryMountPoint> entries;

  std::array<char, 256> mntbuf;
  ::mntent mntent;

  auto defaultHugePageSize = getDefaultHugePageSize();

  while (::getmntent_r(handle, &mntent, mntbuf.data(), mntbuf.size())) {
    if (mntent.mnt_fsname == "tmpfs"sv) {
      auto& entry = entries.emplace_back();
      entry.path = mntent.mnt_dir;
      entry.pageSize = gDefaultPageSize;
      continue;
    }

    if (mntent.mnt_fsname == "hugetlbfs"sv) {
      auto pageSize = getPageSizeFromMountOpts(mntent.mnt_opts);
      if (!pageSize) {
        if (defaultHugePageSize) {
          pageSize = defaultHugePageSize;
        } else {
          fmt::print(stderr, "pagesize option error for mount point \"{}\" ({}): {}\n", mntent.mnt_dir,
              mntent.mnt_fsname, pageSize.assume_error().message());
          continue;
        }
      }

      auto& entry = entries.emplace_back();
      entry.path = mntent.mnt_dir;
      entry.pageSize = pageSize.assume_value();
      continue;
    }
  }

  return entries;
}

std::vector<MemoryMountPoint> const& getProcMounts() {
  static std::vector<MemoryMountPoint> entries = readProcMounts();
  return entries;
}

Result<MemoryMountPoint> getMountEntry1G(std::vector<MemoryMountPoint> const& mounts) noexcept {
  auto const found = std::ranges::find_if(mounts, [](auto const& entry) {
    return entry.pageSize == gPageSize1G;
  });
  if (found == mounts.end()) {
    return makePosixErrorCode(ENOENT);
  }
  return *found;
}

Result<MemoryMountPoint> getMountEntry2M(std::vector<MemoryMountPoint> const& mounts) noexcept {
  auto const found = std::ranges::find_if(mounts, [](auto const& entry) {
    return entry.pageSize == gPageSize2M;
  });
  if (found == mounts.end()) {
    return makePosixErrorCode(ENOENT);
  }
  return *found;
}

Result<MemoryMountPoint> getMountEntryDefault(std::vector<MemoryMountPoint> const& mounts) noexcept {
  using namespace std::string_view_literals;

  auto found = std::ranges::find_if(mounts, [](auto const& entry) {
    return entry.path == "/dev/shm"sv;
  });
  if (found == mounts.end()) {
    found = std::ranges::find_if(mounts, [](auto const& entry) {
      return entry.path == "/tmp"sv;
    });
  }
  if (found == mounts.end()) {
    return makePosixErrorCode(ENOENT);
  }
  return *found;
}

Result<MemoryMountPoint> getMountEntryAuto(std::vector<MemoryMountPoint> const& mounts) noexcept {
  HugePagesOption type = HugePagesOption::HugePages1G;

  for (;;) {
    switch (type) {
    case HugePagesOption::None: {
      return getMountEntryDefault(mounts);
    } break;
    case HugePagesOption::HugePages2M: {
      if (auto result = getMountEntry2M(mounts); result) {
        return result;
      }
      type = HugePagesOption::None;
    } break;
    case HugePagesOption::HugePages1G: {
      if (auto result = getMountEntry1G(mounts); result) {
        return result;
      }
      type = HugePagesOption::HugePages2M;
    } break;
    default: {
      assert(false);
    } break;
    }
  }

  return makePosixErrorCode(ENOENT);
}

} // namespace

DefaultMemorySource::DefaultMemorySource(HugePagesOption hugePagesOpt) {
  Result<MemoryMountPoint> result = makePosixErrorCode(ENOENT);

  switch (hugePagesOpt) {
  case HugePagesOption::Auto: {
    result = getMountEntryAuto(getProcMounts());
  } break;
  case HugePagesOption::None: {
    result = getMountEntryDefault(getProcMounts());
  } break;
  case HugePagesOption::HugePages2M: {
    result = getMountEntry2M(getProcMounts());
  } break;
  case HugePagesOption::HugePages1G: {
    result = getMountEntry1G(getProcMounts());
  } break;
  default: {
    throw std::system_error(EINVAL, getPosixErrorCategory(), "invalid hugePagesOpt value");
  } break;
  }

  if (!result) {
    throw std::system_error(result.assume_error());
  }

  path_ = result.assume_value().path;
  pageSize_ = result.assume_value().pageSize;
}

DefaultMemorySource::DefaultMemorySource(std::filesystem::path const& path, std::size_t pageSize)
    : path_(path), pageSize_(pageSize) {
  if (!exists(path)) {
    throw std::system_error(ENOENT, getPosixErrorCategory(), "directory not exists");
  }
  if (!std::has_single_bit(pageSize)) {
    throw std::system_error(EINVAL, getPosixErrorCategory(), "page size must be power of two");
  }
}

Result<std::tuple<File, std::size_t>> DefaultMemorySource::open(std::string_view name, OpenFlags flags) const noexcept {
  if (flags != OpenFlags::OpenOnly && flags != OpenFlags::OpenOrCreate) {
    return makePosixErrorCode(EINVAL);
  }

  try {
    auto const filePath = path_ / name;
    auto file = (flags == OpenFlags::OpenOnly) ? File(kOpenOnly, filePath, OpenMode::ReadWrite)
                                               : File(kOpenOrCreate, filePath, OpenMode::ReadWrite);
    return std::make_tuple(std::move(file), pageSize_);
  } catch (...) {
    return makePosixErrorCode(EFAULT);
  }
}

Result<std::tuple<File, std::size_t>> AnonymousMemorySource::open(
    std::string_view name, [[maybe_unused]] OpenFlags flags) const noexcept {
  auto result = File::anonymous(std::string(name).c_str());
  if (!result) {
    return result.assume_error();
  }
  return std::make_tuple(std::move(result).assume_value(), gDefaultPageSize);
}

} // namespace turboq
