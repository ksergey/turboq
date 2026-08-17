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

    /// Duplicate file descriptor
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
};

} // namespace turboq
