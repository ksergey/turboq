// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

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

#ifndef MFD_CLOEXEC
auto memfd_create(const char* name, unsigned int flags) -> int {
  // Shouldn't work on linux before 3.17
  return syscall(__NR_memfd_create, name, flags);
}
#define MFD_CLOEXEC FD_CLOEXEC
#endif

namespace turboq {
namespace {

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
    throw std::system_error(errno, getPosixErrorCategory(), "open(...)");
  }
  this->reset(fd, true);
}

File::File(CreateOnly, std::filesystem::path const& path, OpenMode openMode, mode_t mode) {
  int fd = ::open(path.c_str(), makeOpenFlags(openMode) | O_CLOEXEC | O_CREAT | O_EXCL);
  if (fd == -1) {
    throw std::system_error(errno, getPosixErrorCategory(), "open(...)");
  }
  ::fchmod(fd, mode);
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
    throw std::system_error(errno, getPosixErrorCategory(), "open(...)");
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

auto File::closeNoThrow() noexcept -> Result<> {
  int const rc = owns_ ? ::close(fd_) : 0;
  release();
  if (rc != 0) {
    return makePosixErrorCode(errno);
  } else {
    return success();
  }
}

void File::close() {
  if (auto const result = closeNoThrow(); !result) {
    throw std::system_error(result.error(), "close(...)");
  }
}

auto File::dup() const noexcept -> Result<File> {
  if (valid()) {
    int fd = ::dup(get());
    if (fd == -1) {
      return makePosixErrorCode(errno);
    }
    return File(fd, true);
  } else {
    return File();
  }
}

auto File::temporary(std::filesystem::path const& path) noexcept -> Result<File> {
  int fd = ::open(path.c_str(), O_TMPFILE | O_CLOEXEC | O_RDWR, 0666);
  if (fd == -1) {
    return makePosixErrorCode(errno);
  }
  return File(fd, true);
}

auto File::anonymous(char const* name) noexcept -> Result<File> {
  int fd = ::memfd_create(name, MFD_CLOEXEC);
  if (fd == -1) {
    return makePosixErrorCode(errno);
  }
  return File(fd, true);
}

void File::lock() {
  doLock(LOCK_EX);
}

auto File::tryLock() -> bool {
  return doTryLock(LOCK_EX);
}

void File::lockShared() {
  doLock(LOCK_SH);
}

auto File::tryLockShared() -> bool {
  return doTryLock(LOCK_SH);
}

void File::unlock() {
  int rc = flockNoInt(get(), LOCK_UN);
  if (rc == -1) {
    throw std::system_error(errno, getPosixErrorCategory(), "flock(...)");
  }
}

auto File::tryGetFileSize() const noexcept -> Result<std::size_t> {
  struct stat st;
  if (::fstat(this->get(), &st) == -1) {
    return makePosixErrorCode(errno);
  }
  return std::size_t(st.st_size);
}

auto File::getFileSize() const -> std::size_t {
  struct stat st;
  if (::fstat(this->get(), &st) == -1) {
    throw std::system_error(errno, getPosixErrorCategory(), "fstat(...)");
  }
  return st.st_size;
}

auto File::tryTruncate(std::size_t size) const noexcept -> Result<> {
  if (::ftruncate(this->get(), size) == -1) {
    return makePosixErrorCode(errno);
  }
  return success();
}

void File::truncate(std::size_t size) const {
  if (::ftruncate(this->get(), size) == -1) {
    throw std::system_error(errno, getPosixErrorCategory(), "ftruncate(...)");
  }
}

void File::doLock(int op) {
  int rc = flockNoInt(get(), op | LOCK_NB);
  if (rc == -1) {
    throw std::system_error(errno, getPosixErrorCategory(), "flock(...)");
  }
}

auto File::doTryLock(int op) -> bool {
  int rc = flockNoInt(get(), op | LOCK_NB);
  if (rc == -1) {
    if (errno != EWOULDBLOCK) {
      throw std::system_error(errno, getPosixErrorCategory(), "flock(...)");
    }
    return false;
  }
  return true;
}

} // namespace turboq
