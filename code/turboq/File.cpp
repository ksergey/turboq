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
int memfd_create(const char* name, unsigned int flags) {
  // Shouldn't work on linux before 3.17
  return syscall(__NR_memfd_create, name, flags);
}
#define MFD_CLOEXEC FD_CLOEXEC
#endif

namespace turboq {
namespace {

constexpr int makeOpenFlags(OpenMode openMode) noexcept {
  int flags = 0;
  if (openMode == OpenMode::ReadOnly) {
    flags = O_RDONLY;
  } else if (openMode == OpenMode::ReadWrite) {
    flags = O_RDWR;
  }
  return flags;
}

TURBOQ_FORCE_INLINE int flockNoInt(int fd, int op) noexcept {
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

int File::release() noexcept {
  int released = fd_;
  fd_ = kInvalidFd;
  owns_ = false;
  return released;
}

Result<> File::closeNoThrow() noexcept {
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

Result<File> File::dup() const noexcept {
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

Result<File> File::temporary(std::filesystem::path const& path) noexcept {
  int fd = ::open(path.c_str(), O_TMPFILE | O_CLOEXEC | O_RDWR, 0666);
  if (fd == -1) {
    return makePosixErrorCode(errno);
  }
  return File(fd, true);
}

Result<File> File::anonymous(char const* name) noexcept {
  int fd = ::memfd_create(name, MFD_CLOEXEC);
  if (fd == -1) {
    return makePosixErrorCode(errno);
  }
  return File(fd, true);
}

void File::lock() {
  doLock(LOCK_EX);
}

bool File::tryLock() {
  return doTryLock(LOCK_EX);
}

void File::lockShared() {
  doLock(LOCK_SH);
}

bool File::tryLockShared() {
  return doTryLock(LOCK_SH);
}

void File::unlock() {
  int rc = flockNoInt(get(), LOCK_UN);
  if (rc == -1) {
    throw std::system_error(errno, getPosixErrorCategory(), "flock(...)");
  }
}

Result<std::size_t> File::tryGetFileSize() const noexcept {
  struct stat st;
  if (::fstat(this->get(), &st) == -1) {
    return makePosixErrorCode(errno);
  }
  return std::size_t(st.st_size);
}

std::size_t File::getFileSize() const {
  struct stat st;
  if (::fstat(this->get(), &st) == -1) {
    throw std::system_error(errno, getPosixErrorCategory(), "fstat(...)");
  }
  return st.st_size;
}

Result<> File::tryTruncate(std::size_t size) const noexcept {
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

TURBOQ_FORCE_INLINE void File::doLock(int op) {
  int rc = flockNoInt(get(), op | LOCK_NB);
  if (rc == -1) {
    throw std::system_error(errno, getPosixErrorCategory(), "flock(...)");
  }
}

TURBOQ_FORCE_INLINE bool File::doTryLock(int op) {
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
