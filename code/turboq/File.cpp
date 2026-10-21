// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#include "File.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cassert>
#include <print>
#include <system_error>

// memfd_create(2) is in the kernel since 3.17, but the libc wrapper came much later: glibc 2.27, and
// Android's Bionic only from API level 30 (Bionic defines MFD_CLOEXEC regardless, so the macro can't
// tell whether the wrapper is declared). Where the wrapper may be missing, call the syscall directly.
#if (defined(__ANDROID__) && __ANDROID_API__ < 30) || !defined(MFD_CLOEXEC)
#include <sys/syscall.h>
#define TURBOQ_MEMFD_CREATE_SYSCALL 1
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U // <linux/memfd.h>
#endif

#ifndef F_OFD_SETLK
// Not yet in this libc's <fcntl.h> (glibc added the constant in 2.20, 2014) -- Linux itself has
// supported the underlying command since kernel 3.15, the same release that introduced it, so
// this is purely a header-vintage gap, not a runtime one.
#define F_OFD_SETLK 37
#endif

#include "Error.h"

namespace turboq {
namespace {

auto createMemFd(char const* name, unsigned int flags) noexcept -> int {
#ifdef TURBOQ_MEMFD_CREATE_SYSCALL
    return static_cast<int>(::syscall(__NR_memfd_create, name, flags));
#else
    return ::memfd_create(name, flags);
#endif
}

constexpr auto makeOpenFlags(OpenMode openMode) noexcept -> int {
    int flags = 0;
    if (openMode == OpenMode::ReadOnly) {
        flags = O_RDONLY;
    } else if (openMode == OpenMode::ReadWrite) {
        flags = O_RDWR;
    }
    return flags;
}

auto flockNoInt(int fd, int op) noexcept -> int {
    int rc;
    do {
        rc = ::flock(fd, op);
    } while (rc == -1 && errno == EINTR);
    return rc;
}

} // namespace

File::File(OpenOnly, std::filesystem::path const& path, OpenMode openMode) {
    int fd = ::open(path.c_str(), makeOpenFlags(openMode) | O_CLOEXEC);
    if (fd == -1) {
        throw std::system_error{makePosixErrorCode(errno), "open(...)"};
    }
    this->reset(fd, true);
}

File::File(CreateOnly, std::filesystem::path const& path, OpenMode openMode, mode_t mode) {
    int fd = ::open(path.c_str(), makeOpenFlags(openMode) | O_CLOEXEC | O_CREAT | O_EXCL, mode);
    if (fd == -1) {
        throw std::system_error{makePosixErrorCode(errno), "open(...)"};
    }
    ::fchmod(fd, mode); // open()'s mode is subject to umask; fchmod() here guarantees the exact requested mode
    this->reset(fd, true);
}

File::File(OpenOrCreate, std::filesystem::path const& path, OpenMode openMode, mode_t mode) {
    int const flags = makeOpenFlags(openMode);
    int fd = -1;
    while (true) {
        fd = ::open(path.c_str(), flags | O_CLOEXEC | O_CREAT | O_EXCL, mode);
        if (fd >= 0) {
            ::fchmod(fd, mode);
        } else if (errno == EEXIST) {
            fd = ::open(path.c_str(), flags);
            if (fd == -1 && errno == ENOENT) {
                continue;
            }
        }
        break;
    }
    if (fd == -1) {
        throw std::system_error{makePosixErrorCode(errno), "open(...)"};
    }
    this->reset(fd, true);
}

File::~File() noexcept {
    if (owns_) {
        auto const fd = fd_;
        if (auto const result = closeNoThrow(); !result) {
            if (result.error().value() == EBADF) {
                std::print(stderr, "turboq: closing fd {}, it may already have been closed\n", fd);
            }
        }
    }
}

auto File::release() noexcept -> int {
    int released = fd_;
    fd_ = kInvalidFd;
    owns_ = false;
    return released;
}

auto File::closeNoThrow() noexcept -> std::expected<void, std::error_code> {
    auto const rc = owns_ ? ::close(fd_) : 0;
    this->release();
    if (rc != 0) {
        return std::unexpected(makePosixErrorCode(errno));
    } else {
        return {};
    }
}

void File::close() {
    if (auto const result = closeNoThrow(); !result) {
        throw std::system_error{result.error(), "close(...)"};
    }
}

auto File::dup() const noexcept -> std::expected<File, std::error_code> {
    if (this->valid()) {
        int const fd = ::dup(this->get());
        if (fd == -1) {
            return std::unexpected(makePosixErrorCode(errno));
        }
        return {File{fd, true}};
    } else {
        return {File{}};
    }
}

auto File::temporary(std::filesystem::path const& path) noexcept -> std::expected<File, std::error_code> {
    int fd = ::open(path.c_str(), O_TMPFILE | O_CLOEXEC | O_RDWR, 0666);
    if (fd == -1) {
        return std::unexpected(makePosixErrorCode(errno));
    }
    return {File{fd, true}};
}

auto File::anonymous(char const* name) noexcept -> std::expected<File, std::error_code> {
    int fd = createMemFd(name, MFD_CLOEXEC);
    if (fd == -1) {
        return std::unexpected(makePosixErrorCode(errno));
    }
    return {File{fd, true}};
}

void File::lock() {
    this->doLock(LOCK_EX);
}

auto File::tryLock() -> bool {
    return this->doTryLock(LOCK_EX);
}

void File::lockShared() {
    this->doLock(LOCK_SH);
}

auto File::tryLockShared() -> bool {
    return this->doTryLock(LOCK_SH);
}

void File::unlock() {
    int rc = flockNoInt(this->get(), LOCK_UN);
    if (rc == -1) {
        throw std::system_error{makePosixErrorCode(errno), "flock(...)"};
    }
}

void File::lockRegion(std::size_t offset, std::size_t size) {
    if (!this->doRegionLock(F_WRLCK, offset, size)) {
        throw std::system_error{makePosixErrorCode(errno), "fcntl(F_OFD_SETLK, ...)"};
    }
}

auto File::tryLockRegion(std::size_t offset, std::size_t size) -> bool {
    return this->doRegionLock(F_WRLCK, offset, size);
}

void File::unlockRegion(std::size_t offset, std::size_t size) {
    if (!this->doRegionLock(F_UNLCK, offset, size)) {
        throw std::system_error{makePosixErrorCode(errno), "fcntl(F_OFD_SETLK, ...)"};
    }
}

auto File::doRegionLock(short lockType, std::size_t offset, std::size_t size) -> bool {
    struct flock fl{};
    fl.l_type = lockType;
    fl.l_whence = SEEK_SET;
    fl.l_start = static_cast<off_t>(offset);
    fl.l_len = static_cast<off_t>(size);
    fl.l_pid = 0; // required to be 0 for F_OFD_* commands

    if (auto const rc = ::fcntl(this->get(), F_OFD_SETLK, &fl); rc == -1) {
        if (errno == EACCES || errno == EAGAIN) {
            return false;
        }
        throw std::system_error{makePosixErrorCode(errno), "fcntl(F_OFD_SETLK, ...)"};
    }
    return true;
}

auto File::tryGetFileSize() const noexcept -> std::expected<std::size_t, std::error_code> {
    struct stat st;
    if (auto const rc = ::fstat(this->get(), &st); rc == -1) {
        return std::unexpected(makePosixErrorCode(errno));
    }
    return {std::size_t(st.st_size)};
}

auto File::getFileSize() const -> std::size_t {
    struct stat st;
    if (auto const rc = ::fstat(this->get(), &st); rc == -1) {
        throw std::system_error{makePosixErrorCode(errno), "fstat(...)"};
    }
    return st.st_size;
}

auto File::tryTruncate(std::size_t size) const noexcept -> std::expected<void, std::error_code> {
    if (auto const rc = ::ftruncate(this->get(), size); rc == -1) {
        return std::unexpected(makePosixErrorCode(errno));
    }
    return {};
}

void File::truncate(std::size_t size) const {
    if (auto const rc = ::ftruncate(this->get(), size); rc == -1) {
        throw std::system_error{makePosixErrorCode(errno), "ftruncate(...)"};
    }
}

void File::doLock(int op) {
    if (auto const rc = flockNoInt(this->get(), op | LOCK_NB); rc == -1) {
        throw std::system_error{makePosixErrorCode(errno), "flock(...)"};
    }
}

auto File::doTryLock(int op) -> bool {
    if (auto const rc = flockNoInt(this->get(), op | LOCK_NB); rc == -1) {
        if (errno != EWOULDBLOCK) {
            throw std::system_error{makePosixErrorCode(errno), "flock(...)"};
        }
        return false;
    }
    return true;
}

} // namespace turboq
