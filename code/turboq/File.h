// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <filesystem>
#include <utility>

#include <turboq/Result.h>

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
  [[nodiscard]] auto get() const noexcept -> int {
    return fd_;
  }

  /// Return true on descriptor initialized.
  [[nodiscard]] auto valid() const noexcept -> bool {
    return fd_ != kInvalidFd;
  }

  /// \see valid()
  [[nodiscard]] explicit operator bool() const noexcept {
    return this->valid();
  }

  /// Returns and releases the descriptor.
  auto release() noexcept -> int;

  /// Close descriptor if owned.
  auto closeNoThrow() noexcept -> Result<>;

  /// Close descriptor if owned. Throws on error.
  void close();

  /// Duplicate file descriptor
  [[nodiscard]] auto dup() const noexcept -> Result<File>;

  /// Create a temporary file.
  [[nodiscard]] static auto temporary(std::filesystem::path const& path = "/tmp") noexcept -> Result<File>;

  /// Create an anonymous file.
  [[nodiscard]] static auto anonymous(char const* name = "") noexcept -> Result<File>;

  /// Lock file.
  void lock();

  /// Try lock file.
  [[nodiscard]] auto tryLock() -> bool;

  /// Shared lock.
  void lockShared();

  /// Try shared lock.
  [[nodiscard]] auto tryLockShared() -> bool;

  /// Unlock file. Throws on error.
  void unlock();

  /// Get file size
  [[nodiscard]] auto tryGetFileSize() const noexcept -> Result<std::size_t>;

  /// Get file size. Throws on error.
  [[nodiscard]] auto getFileSize() const -> std::size_t;

  /// Truncate file
  auto tryTruncate(std::size_t size) const noexcept -> Result<>;

  /// Truncate file, throws on error
  void truncate(std::size_t size) const;

  /// Swap descriptor with another.
  void swap(File& other) noexcept {
    using std::swap;
    swap(fd_, other.fd_);
    swap(owns_, other.owns_);
  }

  /// Swaps the descriptors and ownership.
  friend void swap(File& lhs, File& rhs) noexcept {
    lhs.swap(rhs);
  }

protected:
  void reset(int fd, bool owns) noexcept {
    [[maybe_unused]] auto const result = closeNoThrow();
    fd_ = fd;
    owns_ = owns;
  }

  void doLock(int op);

  auto doTryLock(int op) -> bool;
};

} // namespace turboq
