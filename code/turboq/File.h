// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <filesystem>
#include <utility>

#include <turboq/Result.h>
#include <turboq/platform.h>

namespace turboq {

/// Create only tag.
struct CreateOnly {};
inline constexpr CreateOnly kCreateOnly = {};

/// Open only tag.
struct OpenOnly {};
inline constexpr OpenOnly kOpenOnly = {};

/// Open or create tag.
struct OpenOrCreate {};
inline constexpr OpenOrCreate kOpenOrCreate = {};

/// File open mode.
enum class OpenMode { ReadOnly, ReadWrite };

/// File descriptor wrapper
class File {
private:
  static constexpr int kInvalidFd = -1;

  int fd_ = kInvalidFd;
  bool owns_ = false;

public:
  File(File const&) = delete;
  File& operator=(File const&) = delete;

  File() = default;

  File(File&& other) noexcept : fd_(other.fd_), owns_(other.owns_) {
    other.release();
  }

  File& operator=(File&& other) noexcept {
    [[maybe_unused]] auto const result = closeNoThrow();
    swap(other);
    return *this;
  }

  /// Open file. Throws on open error.
  File(OpenOnly, std::filesystem::path const& path, OpenMode openMode = OpenMode::ReadOnly);

  /// Create file if not exists. Throws on error.
  File(CreateOnly, std::filesystem::path const& path, OpenMode openMode = OpenMode::ReadOnly, mode_t mode = 0666);

  /// Create file if not exists or open otherwise. Throws on error.
  File(OpenOrCreate, std::filesystem::path const& path, OpenMode openMode = OpenMode::ReadOnly, mode_t mode = 0666);

  /// Destructor. Close file descriptor if owns it.
  virtual ~File() noexcept;

  /// Construct from raw descritor.
  /// Become fd owner on owns set to true.
  explicit File(int fd, bool owns = false) noexcept : fd_(fd), owns_(owns) {}

  /// Return native descriptor.
  [[nodiscard]] TURBOQ_FORCE_INLINE int get() const noexcept {
    return fd_;
  }

  /// Return true on descriptor initialized.
  [[nodiscard]] TURBOQ_FORCE_INLINE bool valid() const noexcept {
    return fd_ != kInvalidFd;
  }

  /// \see valid()
  [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
    return valid();
  }

  /// Returns and releases the descriptor.
  int release() noexcept;

  /// Close descriptor if owned.
  Result<> closeNoThrow() noexcept;

  /// Close descriptor if owned. Throws on error.
  void close();

  /// Duplicate file descriptor
  Result<File> dup() const noexcept;

  /// Create a temporary file.
  static Result<File> temporary(std::filesystem::path const& path = "/tmp") noexcept;

  /// Create an anonymous file.
  static Result<File> anonymous(char const* name = "") noexcept;

  /// Lock file.
  void lock();

  /// Try lock file.
  [[nodiscard]] bool tryLock();

  /// Shared lock.
  void lockShared();

  /// Try shared lock.
  [[nodiscard]] bool tryLockShared();

  /// Unlock file. Throws on error.
  void unlock();

  /// Get file size
  Result<std::size_t> tryGetFileSize() const noexcept;

  /// Get file size. Throws on error.
  [[nodiscard]] std::size_t getFileSize() const;

  /// Truncate file
  Result<> tryTruncate(std::size_t size) const noexcept;

  /// Truncate file, throws on error
  void truncate(std::size_t size) const;

  /// Swap descriptor with another.
  TURBOQ_FORCE_INLINE void swap(File& other) noexcept {
    using std::swap;
    swap(fd_, other.fd_);
    swap(owns_, other.owns_);
  }

  /// Swaps the descriptors and ownership.
  TURBOQ_FORCE_INLINE friend void swap(File& lhs, File& rhs) noexcept {
    lhs.swap(rhs);
  }

protected:
  TURBOQ_FORCE_INLINE void reset(int fd, bool owns) noexcept {
    [[maybe_unused]] auto const result = closeNoThrow();
    fd_ = fd;
    owns_ = owns;
  }

  void doLock(int op);

  bool doTryLock(int op);
};

} // namespace turboq
