// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <cstddef>
#include <span>

#include <turboq/File.h>
#include <turboq/platform.h>

namespace turboq {

class MappedRegion {
private:
  std::byte* data_ = nullptr;
  std::size_t size_ = 0;

public:
  MappedRegion(MappedRegion const&) = delete;
  MappedRegion& operator=(MappedRegion const&) = delete;

  /// Move constructor.
  MappedRegion(MappedRegion&& that) noexcept {
    swap(that);
  }

  /// Move assignment.
  MappedRegion& operator=(MappedRegion&& that) noexcept {
    swap(that);
    return *this;
  }

  /// Construct empty object for late initialization.
  MappedRegion() = default;

  /// Construct mapped region from pointer to data and size. Own early mapped
  /// region with mmap.
  MappedRegion(std::byte* data, std::size_t size) noexcept : data_(data), size_(size) {}

  /// Destructor. Unmap mmaped memory if owns it.
  virtual ~MappedRegion() noexcept;

  /// Return true if initialized.
  [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
    return size_ > 0;
  }

  /// Return pointer to data.
  [[nodiscard]] TURBOQ_FORCE_INLINE std::byte const* data() const noexcept {
    return data_;
  }

  /// \overload
  [[nodiscard]] TURBOQ_FORCE_INLINE std::byte* data() noexcept {
    return data_;
  }

  /// Return size of data.
  [[nodiscard]] TURBOQ_FORCE_INLINE std::size_t size() const noexcept {
    return size_;
  }

  /// Return mapped region content
  [[nodiscard]] TURBOQ_FORCE_INLINE std::span<std::byte const> content() const noexcept {
    return {data_, size_};
  }

  /// \overload
  [[nodiscard]] TURBOQ_FORCE_INLINE std::span<std::byte> content() noexcept {
    return {data_, size_};
  }

  /// Swap resources with other MappedRegion object.
  void swap(MappedRegion& that) noexcept {
    using std::swap;
    swap(data_, that.data_);
    swap(size_, that.size_);
  }

  /// Swap to MappedRegion objects.
  friend void swap(MappedRegion& a, MappedRegion& b) noexcept {
    a.swap(b);
  }
};

} // namespace turboq
