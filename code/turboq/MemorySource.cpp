// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#include "MemorySource.h"

#include <mntent.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <charconv>
#include <cstdio>
#include <ranges>
#include <regex>
#include <string_view>
#include <system_error>
#include <vector>

#include "Error.h"
#include "ScopeGuard.h"

namespace turboq {
namespace detail {

auto getDefaultPageSize() noexcept -> std::size_t {
    static const auto pageSize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    return pageSize;
}

} // namespace detail

namespace {

constexpr auto kPageSize2M = static_cast<std::size_t>(2 * 1024 * 1024);
constexpr auto kPageSize1G = static_cast<std::size_t>(1 * 1024 * 1024 * 1024);

auto getDefaultHugePageSize() noexcept -> std::expected<std::size_t, std::error_code> {
    using namespace std::string_view_literals;

    static auto const regex = std::regex{R"!(Hugepagesize:\s*(\d+)\s*kB)!"};

    auto handle = ::fopen("/proc/meminfo", "r");
    if (!handle) {
        return std::unexpected(makePosixErrorCode(errno));
    }

    char* line = nullptr;
    std::size_t len = 0;

    ScopeGuard guard{[&]() noexcept {
        ::fclose(handle);
        if (line) {
            ::free(line);
        }
    }};

    std::cmatch match;

    while (::getline(&line, &len, handle) != -1) {
        auto input = std::string_view{line, ::strlen(line) - 1};

        if (!std::regex_match(input.begin(), input.end(), match, regex)) {
            continue;
        }

        std::size_t pageSizeKiB;
        auto const rc = std::from_chars(match[1].first, match[1].second, pageSizeKiB);
        if (rc.ec == std::errc()) {
            return {pageSizeKiB * 1024};
        } else {
            return std::unexpected(makePosixErrorCode(EINVAL));
        }
    }

    return std::unexpected(makePosixErrorCode(ENOENT));
}

auto getPageSizeFromMountOpts(std::string_view opts) noexcept -> std::expected<std::size_t, std::error_code> {
    using namespace std::string_view_literals;

    for (auto const word : std::views::split(std::string_view{opts}, ","sv)) {
        auto const option = std::string_view(std::ranges::data(word), std::ranges::size(word));
        if (!option.starts_with("pagesize="sv)) {
            continue;
        }
        std::string_view value = option.substr("pagesize="sv.size());
        if (value == "2M"sv) {
            return {kPageSize2M};
        } else if (value == "1G"sv) {
            return {kPageSize1G};
        } else {
            return std::unexpected(makePosixErrorCode(EINVAL));
        }
    }

    return std::unexpected(makePosixErrorCode(ENOENT));
}

struct MemoryMountPoint {
    std::filesystem::path path;
    std::size_t pageSize;
};

auto readProcMounts() -> std::vector<MemoryMountPoint> {
    using namespace std::string_view_literals;

    auto handle = ::setmntent("/proc/mounts", "r");
    if (!handle) {
        throw std::system_error{makePosixErrorCode(ENOENT), "setmntent(...)"};
    }

    ScopeGuard guard{[&]() noexcept {
        ::endmntent(handle);
    }};

    std::vector<MemoryMountPoint> entries;

    std::array<char, 256> mntbuf;
    ::mntent mntent;

    auto defaultHugePageSize = getDefaultHugePageSize();

    while (::getmntent_r(handle, &mntent, mntbuf.data(), mntbuf.size())) {
        if (mntent.mnt_fsname == "tmpfs"sv) {
            auto& entry = entries.emplace_back();
            entry.path = mntent.mnt_dir;
            entry.pageSize = detail::getDefaultPageSize();
            continue;
        }

        if (mntent.mnt_fsname == "hugetlbfs"sv) {
            auto pageSize = getPageSizeFromMountOpts(mntent.mnt_opts);
            if (!pageSize) {
                if (defaultHugePageSize) {
                    pageSize = defaultHugePageSize;
                } else {
                    std::fprintf(stderr, "turboq: pagesize option error for mount point \"%s\" (%s): %s\n",
                        mntent.mnt_dir, mntent.mnt_fsname, pageSize.error().message().c_str());
                    continue;
                }
            }

            auto& entry = entries.emplace_back();
            entry.path = mntent.mnt_dir;
            entry.pageSize = pageSize.value();
            continue;
        }
    }

    return entries;
}

auto getProcMounts() -> std::vector<MemoryMountPoint> const& {
    static std::vector<MemoryMountPoint> entries = readProcMounts();
    return entries;
}

auto getMountEntry1G(
    std::vector<MemoryMountPoint> const& mounts) noexcept -> std::expected<MemoryMountPoint, std::error_code> {
    auto const found = std::ranges::find_if(mounts, [](auto const& entry) {
        return entry.pageSize == kPageSize1G;
    });
    if (found == mounts.end()) {
        return std::unexpected(makePosixErrorCode(ENOENT));
    }
    return {*found};
}

auto getMountEntry2M(
    std::vector<MemoryMountPoint> const& mounts) noexcept -> std::expected<MemoryMountPoint, std::error_code> {
    auto const found = std::ranges::find_if(mounts, [](auto const& entry) {
        return entry.pageSize == kPageSize2M;
    });
    if (found == mounts.end()) {
        return std::unexpected(makePosixErrorCode(ENOENT));
    }
    return {*found};
}

auto getMountEntryDefault(
    std::vector<MemoryMountPoint> const& mounts) noexcept -> std::expected<MemoryMountPoint, std::error_code> {
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
        return std::unexpected(makePosixErrorCode(ENOENT));
    }
    return {*found};
}

auto getMountEntryAuto(
    std::vector<MemoryMountPoint> const& mounts) noexcept -> std::expected<MemoryMountPoint, std::error_code> {
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

    return std::unexpected(makePosixErrorCode(ENOENT));
}

} // namespace

DefaultMemorySource::DefaultMemorySource(HugePagesOption hugePagesOpt) {
    std::expected<MemoryMountPoint, std::error_code> result{std::unexpected(makePosixErrorCode(ENOENT))};

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
        throw std::system_error{makePosixErrorCode(EINVAL), "invalid hugePagesOpt value"};
    } break;
    }

    if (!result) {
        throw std::system_error{result.error()};
    }

    path_ = result.value().path;
    pageSize_ = result.value().pageSize;
}

DefaultMemorySource::DefaultMemorySource(std::filesystem::path const& path, std::size_t pageSize)
    : path_{path}, pageSize_{pageSize} {
    if (!std::filesystem::exists(path_)) {
        throw std::system_error{makePosixErrorCode(ENOENT), "directory not exists"};
    }
    if (!std::has_single_bit(pageSize_)) {
        throw std::system_error{makePosixErrorCode(EINVAL), "page size must be power of two"};
    }
}

auto DefaultMemorySource::open(std::string_view name,
    OpenFlags flags) const noexcept -> std::expected<std::tuple<File, std::size_t>, std::error_code> {
    if (flags != OpenFlags::OpenOnly && flags != OpenFlags::OpenOrCreate) {
        return std::unexpected(makePosixErrorCode(EINVAL));
    }

    auto const filePath = path_ / name;
    auto result = (flags == OpenFlags::OpenOnly) ? File::makeFile(kOpenOnly, filePath, OpenMode::ReadWrite)
                                                 : File::makeFile(kOpenOrCreate, filePath, OpenMode::ReadWrite);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {std::make_tuple(std::move(result).value(), pageSize_)};
}

auto AnonymousMemorySource::open(std::string_view name,
    [[maybe_unused]] OpenFlags flags) const noexcept -> std::expected<std::tuple<File, std::size_t>, std::error_code> {
    auto result = File::anonymous(std::string{name}.c_str());
    if (!result) {
        return std::unexpected(result.error());
    }
    return {std::make_tuple(std::move(result).value(), detail::getDefaultPageSize())};
}

} // namespace turboq
