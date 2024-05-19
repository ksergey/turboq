// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

#include <turboq/MappedRegion.h>
#include <turboq/MemorySource.h>
#include <turboq/detail/math.h>
#include <turboq/detail/mmap.h>
#include <turboq/platform.h>

#include <fmt/format.h>

namespace turboq {
namespace internal {

/// Circular FIFO byte queue details
struct BoundedSPSCRawQueueDetail {
  /// Queue tag
  static constexpr auto kTag = std::string_view("turboq/SPSC");

  /// Control struct for queue buffer
  struct MemoryHeader {
    /// Placeholder for queue tag
    char tag[kTag.size()];
    /// Producer position
    alignas(kHardwareDestructiveInterferenceSize) std::size_t producerPos;
    /// Consumer position
    alignas(kHardwareDestructiveInterferenceSize) std::size_t consumerPos;

    static_assert(std::atomic_ref<std::size_t>::is_always_lock_free);
  };

  /// Control struct for message
  struct MessageHeader {
    std::size_t size;
    std::size_t payloadOffset;
    std::size_t payloadSize;
  };

  /// Offset for the first message header from memory buffer start.
  static constexpr std::size_t kDataOffset = detail::ceil(sizeof(MemoryHeader), kHardwareDestructiveInterferenceSize);

  /// Check buffer points to valid SPMC queue region
  /// Return true on success and false otherwise.
  [[nodiscard]] static bool check(std::span<std::byte const> buffer) noexcept {
    if (buffer.size() < kDataOffset + 1) {
      return false;
    }
    auto const header = std::bit_cast<MemoryHeader const*>(buffer.data());
    if (!std::equal(kTag.begin(), kTag.end(), header->tag)) {
      return false;
    }
    return true;
  }

  /// Init queue memory header
  void init(std::span<std::byte> buffer) noexcept {
    auto header = std::bit_cast<MemoryHeader*>(buffer.data());
    std::copy(kTag.begin(), kTag.end(), header->tag);
    std::atomic_ref(header->producerPos).store(0, std::memory_order_relaxed);
    std::atomic_ref(header->consumerPos).store(0, std::memory_order_relaxed);
  }
};

/// Implements a circular FIFO byte queue producer
class BoundedSPSCRawQueueProducer : BoundedSPSCRawQueueDetail {
private:
  MappedRegion storage_;
  MemoryHeader* header_ = nullptr;
  std::span<std::byte> data_;
  std::size_t producerPosCache_ = 0;
  std::size_t minFreeSpace_ = 0;
  MessageHeader* lastMessageHeader_ = nullptr;

public:
  BoundedSPSCRawQueueProducer() = default;
  ~BoundedSPSCRawQueueProducer() = default;

  BoundedSPSCRawQueueProducer(BoundedSPSCRawQueueProducer&& that) noexcept {
    swap(that);
  }

  BoundedSPSCRawQueueProducer& operator=(BoundedSPSCRawQueueProducer&& that) noexcept {
    swap(that);
    return *this;
  }

  BoundedSPSCRawQueueProducer(MappedRegion&& storage) : storage_(std::move(storage)) {
    auto content = storage_.content();

    if (!check(content)) {
      throw std::runtime_error("invalid queue");
    }

    header_ = std::bit_cast<MemoryHeader*>(content.data());
    data_ = content.subspan(kDataOffset);
    producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);

    auto const consumerPos = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);
    if (consumerPos > producerPosCache_) {
      // queue is empty in case of consumerPos == producerPos
      minFreeSpace_ = consumerPos - producerPosCache_ - 1;
    } else {
      // Reserve space at end for last MessageHeader
      minFreeSpace_ = data_.size() - producerPosCache_ - sizeof(MessageHeader);
    }
  }

  /// Return true on initialized
  [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
    return static_cast<bool>(storage_);
  }

  /// Reserve contiguous space for writing without making it visible to the
  /// consumers. Return empty buffer on error
  [[nodiscard]] TURBOQ_FORCE_INLINE std::span<std::byte> prepare(std::size_t size) noexcept {
    std::size_t const alignedSize = detail::ceil(size + sizeof(MessageHeader), kHardwareDestructiveInterferenceSize);

    if (alignedSize <= minFreeSpace_) [[likely]] {
      lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
      lastMessageHeader_->size = alignedSize - sizeof(MessageHeader);
      lastMessageHeader_->payloadSize = size;
      lastMessageHeader_->payloadOffset = producerPosCache_ + sizeof(MessageHeader);
      producerPosCache_ += alignedSize;
      minFreeSpace_ -= alignedSize;

      return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
    }

    auto const consumerPosCache = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);

    if (consumerPosCache > producerPosCache_) {
      // queue is empty in case of consumerPos == producerPos
      minFreeSpace_ = consumerPosCache - producerPosCache_ - 1;

      if (alignedSize <= minFreeSpace_) [[likely]] {
        lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
        lastMessageHeader_->size = alignedSize - sizeof(MessageHeader);
        lastMessageHeader_->payloadSize = size;
        lastMessageHeader_->payloadOffset = producerPosCache_ + sizeof(MessageHeader);
        producerPosCache_ += alignedSize;
        minFreeSpace_ -= alignedSize;

        return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
      }
    } else {
      assert(sizeof(MessageHeader) <= (data_.size() - producerPosCache_));

      minFreeSpace_ = data_.size() - producerPosCache_ - sizeof(MessageHeader);

      if (alignedSize <= minFreeSpace_) [[likely]] {
        lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
        lastMessageHeader_->size = alignedSize - sizeof(MessageHeader);
        lastMessageHeader_->payloadSize = size;
        lastMessageHeader_->payloadOffset = producerPosCache_ + sizeof(MessageHeader);
        producerPosCache_ += alignedSize;
        minFreeSpace_ -= alignedSize;

        return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
      }

      // align payload to cache-line size when payload starts from begining
      std::size_t const alignedSize2 = detail::ceil(size, kHardwareDestructiveInterferenceSize);
      if (alignedSize2 < consumerPosCache) {
        lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
        lastMessageHeader_->size = alignedSize2;
        lastMessageHeader_->payloadSize = size;
        lastMessageHeader_->payloadOffset = 0;
        producerPosCache_ = lastMessageHeader_->size;
        minFreeSpace_ = consumerPosCache - producerPosCache_ - 1;

        return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
      }
    }

    return {};
  }

  /// Make reserved buffer visible for consumers
  TURBOQ_FORCE_INLINE void commit() noexcept {
    std::atomic_ref(header_->producerPos).store(producerPosCache_, std::memory_order_release);
  }

  /// \overload
  TURBOQ_FORCE_INLINE void commit(std::size_t size) noexcept {
    if (size <= lastMessageHeader_->payloadSize) [[likely]] {
      lastMessageHeader_->payloadSize = size;
    } else {
      assert(false);
    }
    commit();
  }

  /// Swap resources with other producer
  void swap(BoundedSPSCRawQueueProducer& that) noexcept {
    using std::swap;

    swap(storage_, that.storage_);
    swap(header_, that.header_);
    swap(data_, that.data_);
    swap(producerPosCache_, that.producerPosCache_);
    swap(minFreeSpace_, that.minFreeSpace_);
    swap(lastMessageHeader_, that.lastMessageHeader_);
  }

  /// \see BoundedSPSCRawQueueProducer::swap
  friend void swap(BoundedSPSCRawQueueProducer& a, BoundedSPSCRawQueueProducer& b) noexcept {
    a.swap(b);
  }
};

class BoundedSPSCRawQueueConsumer : BoundedSPSCRawQueueDetail {
private:
  MappedRegion storage_;
  MemoryHeader* header_ = nullptr;
  std::span<std::byte> data_;
  std::size_t consumerPosCache_ = 0;
  std::size_t producerPosCache_ = 0;
  MessageHeader* lastMessageHeader_ = nullptr;

public:
  BoundedSPSCRawQueueConsumer() = default;
  ~BoundedSPSCRawQueueConsumer() = default;

  BoundedSPSCRawQueueConsumer(BoundedSPSCRawQueueConsumer&& that) noexcept {
    swap(that);
  }

  BoundedSPSCRawQueueConsumer& operator=(BoundedSPSCRawQueueConsumer&& that) noexcept {
    swap(that);
    return *this;
  }

  BoundedSPSCRawQueueConsumer(MappedRegion&& storage) : storage_(std::move(storage)) {
    auto content = storage_.content();

    if (!check(content)) {
      throw std::runtime_error("invalid queue");
    }

    header_ = std::bit_cast<MemoryHeader*>(content.data());
    data_ = content.subspan(kDataOffset);
    consumerPosCache_ = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);
    producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
  }

  /// Return true on initialized
  [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
    return static_cast<bool>(storage_);
  }

  /// Get next buffer for reading. Return empty buffer in case of no data.
  [[nodiscard]] TURBOQ_FORCE_INLINE std::span<std::byte const> fetch() noexcept {
    if ((consumerPosCache_ == producerPosCache_ &&
            (producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire)) ==
                consumerPosCache_)) [[unlikely]] {
      return {};
    }

    lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + consumerPosCache_);

    return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
  }

  /// Consume front buffer and make buffer available for producer
  /// pre: fetch() -> non empty buffer
  TURBOQ_FORCE_INLINE void consume() noexcept {
    consumerPosCache_ = lastMessageHeader_->payloadOffset + lastMessageHeader_->size;
    std::atomic_ref(header_->consumerPos).store(consumerPosCache_, std::memory_order_release);
  }

  /// Reset queue
  TURBOQ_FORCE_INLINE void reset() noexcept {
    producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
    consumerPosCache_ = producerPosCache_;
    std::atomic_ref(header_->consumerPos).store(consumerPosCache_, std::memory_order_release);
  }

  /// Swap resources with other object
  void swap(BoundedSPSCRawQueueConsumer& that) noexcept {
    using std::swap;
    swap(storage_, that.storage_);
    swap(header_, that.header_);
    swap(data_, that.data_);
    swap(consumerPosCache_, that.consumerPosCache_);
    swap(producerPosCache_, that.producerPosCache_);
    swap(lastMessageHeader_, that.lastMessageHeader_);
  }

  /// \see BoundedSPSCRawQueueConsumer::swap
  friend void swap(BoundedSPSCRawQueueConsumer& a, BoundedSPSCRawQueueConsumer& b) noexcept {
    a.swap(b);
  }
};

} // namespace internal

class BoundedSPSCRawQueue : internal::BoundedSPSCRawQueueDetail {
private:
  File file_;

public:
  using Producer = internal::BoundedSPSCRawQueueProducer;
  using Consumer = internal::BoundedSPSCRawQueueConsumer;

  struct CreationOptions {
    std::size_t sizeHint;
  };

  BoundedSPSCRawQueue(BoundedSPSCRawQueue const&) = delete;
  BoundedSPSCRawQueue& operator=(BoundedSPSCRawQueue const&) = delete;
  BoundedSPSCRawQueue() = default;

  BoundedSPSCRawQueue(BoundedSPSCRawQueue&& that) noexcept {
    swap(that);
  }

  BoundedSPSCRawQueue& operator=(BoundedSPSCRawQueue&& that) noexcept {
    swap(that);
    return *this;
  }

  /// Open only queue. Throws on error.
  BoundedSPSCRawQueue(std::string_view name, MemorySource const& memorySource = DefaultMemorySource()) {
    auto result = memorySource.open(name, MemorySource::OpenOnly);
    if (!result) {
      throw std::runtime_error("failed to open memory source");
    }
    std::size_t pageSize;
    std::tie(file_, pageSize) = std::move(result).assume_value();

    if (auto storage = detail::mapFile(file_); !check(storage.content())) {
      throw std::runtime_error("failed to open queue (invalid)");
    }
  }

  /// Open or create queue. Throws on error.
  BoundedSPSCRawQueue(
      std::string_view name, CreationOptions const& options, MemorySource const& memorySource = DefaultMemorySource()) {
    if (options.sizeHint < kDataOffset) {
      throw std::runtime_error("invalid argument (size)");
    }
    auto result = memorySource.open(name, MemorySource::OpenOrCreate);
    if (!result) {
      throw std::runtime_error("failed to open memory source");
    }

    std::size_t pageSize;
    std::tie(file_, pageSize) = std::move(result).assume_value();

    // round-up requested size to page size
    std::size_t const size = detail::ceil(options.sizeHint, pageSize);

    // init queue or check queue's options is the same as requested
    if (auto const fileSize = file_.getFileSize(); fileSize != 0) {
      if (fileSize != size) {
        throw std::runtime_error("size mismatch");
      }
      if (auto storage = detail::mapFile(file_); !check(storage.content())) {
        throw std::runtime_error("failed to open queue (invalid)");
      }
    } else {
      file_.truncate(size);
      init(detail::mapFile(file_, size).content());
    }
  }

  /// Return true on queue intialized.
  [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
    return static_cast<bool>(file_);
  }

  [[nodiscard]] TURBOQ_FORCE_INLINE Producer createProducer() {
    if (!operator bool()) {
      throw std::runtime_error("queue not initialized");
    }
    return Producer(detail::mapFile(file_));
  }

  [[nodiscard]] TURBOQ_FORCE_INLINE Consumer createConsumer() {
    if (!operator bool()) {
      throw std::runtime_error("queue not initialized");
    }
    if (!file_.tryLock()) {
      throw std::runtime_error("can't create consumer (already exists?)");
    }
    return Consumer(detail::mapFile(file_));
  }

  /// Swap resources with other queue.
  void swap(BoundedSPSCRawQueue& that) noexcept {
    using std::swap;
    swap(file_, that.file_);
  }

  /// \see BoundedSPSCRawQueue::swap
  friend void swap(BoundedSPSCRawQueue& a, BoundedSPSCRawQueue& b) noexcept {
    a.swap(b);
  }
};

} // namespace turboq
