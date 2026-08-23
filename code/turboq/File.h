// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <filesystem>
#include <utility>

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

    int fd_{kInvalidFd};
    bool owns_{false};

public:
    File(File const&) = delete;
    File& operator=(File const&) = delete;

    File() = default;

    File(File&& other) noexcept : fd_{std::exchange(other.fd_, kInvalidFd)}, owns_{std::exchange(other.owns_, false)} {}

    File& operator=(File&& other) noexcept {
        if (this != &other) {
            this->~File();
            new (this) File{std::move(other)};
        }
        return *this;
    }

    /// Open file, throws std::system_error on error.
    File(OpenOnly, std::filesystem::path const& path, OpenMode openMode = OpenMode::ReadOnly);

    /// Create file, throws std::system_error on error.
    File(CreateOnly, std::filesystem::path const& path, OpenMode openMode = OpenMode::ReadOnly, mode_t mode = 0666);

    /// Open or create file, throws std::system_error on error.
    File(OpenOrCreate, std::filesystem::path const& path, OpenMode openMode = OpenMode::ReadOnly, mode_t mode = 0666);

    /// Destructor. Close file descriptor if owns it.
    virtual ~File() noexcept;

    /// Construct from raw descritor.
    /// Become fd owner on owns set to true.
    explicit File(int fd, bool owns = false) noexcept : fd_{fd}, owns_{owns} {}

    /// Exception safe constructors
    template <typename... Args>
    static auto makeFile(Args&&... args) noexcept -> std::expected<File, std::error_code> {
        try {
            return {File{std::forward<Args>(args)...}};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

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
    auto closeNoThrow() noexcept -> std::expected<void, std::error_code>;

    /// Close descriptor if owned. Throws on error.
    void close();

    /// Duplicate file descriptor (unused)
    [[nodiscard]] auto dup() const noexcept -> std::expected<File, std::error_code>;

    /// Create a temporary file.
    [[nodiscard]] static auto temporary(std::filesystem::path const& path = "/tmp") noexcept
        -> std::expected<File, std::error_code>;

    /// Create an anonymous file.
    [[nodiscard]] static auto anonymous(char const* name = "") noexcept -> std::expected<File, std::error_code>;

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

    /// Try to acquire an exclusive lock on a byte range [offset, offset + size) within this file,
    /// via a Linux Open File Description lock (fcntl F_OFD_SETLK) rather than lock()/tryLock()'s
    /// flock(), which locks the WHOLE file. Because this locks only that range, a single fd can
    /// hold several independent locks at once by using disjoint ranges -- e.g. one for "am I the
    /// only producer", another for "am I the only consumer", both on the same open file.
    ///
    /// And unlike classic POSIX record locks (fcntl F_SETLK), this is scoped to THIS open file
    /// description, the same way flock() is: closing some unrelated fd that happens to reference
    /// the same file does NOT release it. That distinction matters whenever a queue's producer
    /// and consumer share one File within the same process -- with classic F_SETLK, closing
    /// either one's fd would silently drop *both* locks, even though the other fd is still open.
    [[nodiscard]] auto tryLockRegion(std::size_t offset, std::size_t size = 1) -> bool;

    /// \see tryLockRegion(). Throws if the lock is already held.
    void lockRegion(std::size_t offset, std::size_t size = 1);

    /// Release a lock taken by tryLockRegion()/lockRegion(). offset and size must match what was
    /// locked. Throws on error.
    void unlockRegion(std::size_t offset, std::size_t size = 1);

    /// Get file size
    [[nodiscard]] auto tryGetFileSize() const noexcept -> std::expected<std::size_t, std::error_code>;

    /// Get file size. Throws on error.
    [[nodiscard]] auto getFileSize() const -> std::size_t;

    /// Truncate file
    auto tryTruncate(std::size_t size) const noexcept -> std::expected<void, std::error_code>;

    /// Truncate file, throws on error
    void truncate(std::size_t size) const;

protected:
    void reset(int fd, bool owns) noexcept {
        [[maybe_unused]] auto const result = this->closeNoThrow();
        fd_ = fd;
        owns_ = owns;
    }

    void doLock(int op);

    auto doTryLock(int op) -> bool;

    auto doRegionLock(short lockType, std::size_t offset, std::size_t size) -> bool;
};

} // namespace turboq
