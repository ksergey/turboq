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

namespace turboq {
namespace internal {

/// Circular FIFO byte queue details
struct BoundedSPMCRawQueueDetail {
  /// Queue tag
  static constexpr auto kTag = std::string_view("turboq/SPMC");

  /// Control struct for queue buffer
  struct MemoryHeader {
    /// Placeholder for queue tag
    char tag[kTag.size()];
    /// Producer position
    alignas(kHardwareDestructiveInterferenceSize) std::atomic_size_t producerPos;
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
  }
};

/// Implements a circular FIFO byte queue producer
class BoundedSPMCRawQueueProducer : BoundedSPMCRawQueueDetail {
private:
  MappedRegion storage_;
  std::span<std::byte> data_;
  MemoryHeader* header_ = nullptr;
  std::size_t producerPosCache_ = 0;
  MessageHeader* lastMessageHeader_ = nullptr;

public:
  BoundedSPMCRawQueueProducer() = default;
  ~BoundedSPMCRawQueueProducer() = default;

  BoundedSPMCRawQueueProducer(BoundedSPMCRawQueueProducer&& that) noexcept {
    swap(that);
  }

  BoundedSPMCRawQueueProducer& operator=(BoundedSPMCRawQueueProducer&& that) noexcept {
    swap(that);
    return *this;
  }

  BoundedSPMCRawQueueProducer(MappedRegion&& storage) : storage_(std::move(storage)) {
    auto content = storage_.content();

    if (!check(content)) {
      throw std::runtime_error("invalid queue");
    }

    header_ = std::bit_cast<MemoryHeader*>(storage_.data());
    data_ = content.subspan(kDataOffset);
    producerPosCache_ = header_->producerPos.load(std::memory_order_acquire);
  }

  /// Return true on initialized
  [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
    return static_cast<bool>(storage_);
  }

  /// Return queue capacity (bytes)
  [[nodiscard]] TURBOQ_FORCE_INLINE std::size_t capacity() const noexcept {
    return storage_.size();
  }

  /// Reserve contiguous space for writing without making it visible to the consumers
  [[nodiscard]] TURBOQ_FORCE_INLINE std::span<std::byte> prepare(std::size_t size) noexcept {
    std::size_t const alignedSize = detail::ceil(size + sizeof(MessageHeader), kHardwareDestructiveInterferenceSize);

    lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
    lastMessageHeader_->size = alignedSize - sizeof(MessageHeader);
    lastMessageHeader_->payloadSize = size;

    if (producerPosCache_ + alignedSize + sizeof(MessageHeader) > data_.size()) [[unlikely]] {
      producerPosCache_ = 0;
    } else {
      producerPosCache_ += sizeof(MessageHeader);
    }

    lastMessageHeader_->payloadOffset = producerPosCache_;
    producerPosCache_ += lastMessageHeader_->size;

    return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
  }

  /// Make reserved buffer visible for consumers
  TURBOQ_FORCE_INLINE void commit() noexcept {
    header_->producerPos.store(producerPosCache_, std::memory_order_release);
  }

  /// \overload
  TURBOQ_FORCE_INLINE void commit(std::size_t size) noexcept {
    // Update payload size
    if (size <= lastMessageHeader_->payloadSize) [[likely]] {
      lastMessageHeader_->payloadSize = size;
    } else {
      assert(false);
    }
    commit();
  }

  /// Swap resources with other producer
  void swap(BoundedSPMCRawQueueProducer& that) noexcept {
    using std::swap;
    swap(storage_, that.storage_);
    swap(data_, that.data_);
    swap(header_, that.header_);
    swap(producerPosCache_, that.producerPosCache_);
    swap(lastMessageHeader_, that.lastMessageHeader_);
  }

  /// \see BoundedSPMCRawQueueProducer::swap
  friend void swap(BoundedSPMCRawQueueProducer& a, BoundedSPMCRawQueueProducer& b) noexcept {
    a.swap(b);
  }
};

/// Implements a circular FIFO byte queue consumer
class BoundedSPMCRawQueueConsumer : BoundedSPMCRawQueueDetail {
private:
  MappedRegion storage_;
  std::span<std::byte> data_;
  MemoryHeader* header_ = nullptr;
  std::size_t consumerPosCache_ = 0;
  std::size_t producerPosCache_ = 0;

public:
  BoundedSPMCRawQueueConsumer() = default;
  ~BoundedSPMCRawQueueConsumer() = default;

  BoundedSPMCRawQueueConsumer(BoundedSPMCRawQueueConsumer&& that) noexcept {
    swap(that);
  }

  BoundedSPMCRawQueueConsumer& operator=(BoundedSPMCRawQueueConsumer&& that) noexcept {
    swap(that);
    return *this;
  }

  BoundedSPMCRawQueueConsumer(MappedRegion&& storage) : storage_(std::move(storage)) {
    auto content = storage_.content();

    if (!check(content)) {
      throw std::runtime_error("invalid queue");
    }

    header_ = std::bit_cast<MemoryHeader*>(content.data());
    data_ = content.subspan(kDataOffset);
    consumerPosCache_ = header_->producerPos.load(std::memory_order_relaxed);
    producerPosCache_ = consumerPosCache_;
  }

  /// Return true on initialized
  [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
    return static_cast<bool>(storage_);
  }

  /// Return queue capacity
  [[nodiscard]] TURBOQ_FORCE_INLINE std::size_t capacity() const noexcept {
    return storage_.size();
  }

  /// Get next buffer for reading. Return empty buffer in case of no data.
  [[nodiscard]] TURBOQ_FORCE_INLINE std::span<std::byte const> fetch() noexcept {
    if (producerPosCache_ == consumerPosCache_ &&
        (producerPosCache_ = header_->producerPos.load(std::memory_order_acquire)) == consumerPosCache_) {
      return {};
    }

    MessageHeader const* header = std::bit_cast<MessageHeader*>(data_.data() + consumerPosCache_);
    consumerPosCache_ = header->payloadOffset + header->size;

    return {data_.data() + header->payloadOffset, header->payloadSize};
  }

  /// Do nothing. Just for interface.
  TURBOQ_FORCE_INLINE void consume() noexcept {}

  /// Reset queue
  TURBOQ_FORCE_INLINE void reset() noexcept {
    consumerPosCache_ = header_->producerPos.load(std::memory_order_relaxed);
    producerPosCache_ = consumerPosCache_;
  }

  /// Swap resources with other object
  void swap(BoundedSPMCRawQueueConsumer& that) noexcept {
    using std::swap;
    swap(storage_, that.storage_);
    swap(data_, that.data_);
    swap(header_, that.header_);
    swap(consumerPosCache_, that.consumerPosCache_);
    swap(producerPosCache_, that.producerPosCache_);
  }

  /// \see BoundedSPMCRawQueueConsumer::swap
  friend void swap(BoundedSPMCRawQueueConsumer& a, BoundedSPMCRawQueueConsumer& b) noexcept {
    a.swap(b);
  }
};

} // namespace internal

class BoundedSPMCRawQueue : internal::BoundedSPMCRawQueueDetail {
private:
  File file_;

public:
  using Producer = internal::BoundedSPMCRawQueueProducer;
  using Consumer = internal::BoundedSPMCRawQueueConsumer;

  struct CreationOptions {
    std::size_t sizeHint;
  };

  BoundedSPMCRawQueue(BoundedSPMCRawQueue const&) = delete;
  BoundedSPMCRawQueue& operator=(BoundedSPMCRawQueue const&) = delete;
  BoundedSPMCRawQueue() = default;

  BoundedSPMCRawQueue(BoundedSPMCRawQueue&& that) noexcept {
    swap(that);
  }

  BoundedSPMCRawQueue& operator=(BoundedSPMCRawQueue&& that) noexcept {
    swap(that);
    return *this;
  }

  /// Open only queue. Throws on error.
  BoundedSPMCRawQueue(std::string_view name, MemorySource const& memorySource = DefaultMemorySource()) {
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
  BoundedSPMCRawQueue(
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

  /// Create producer for the queue. Throws on error.
  [[nodiscard]] TURBOQ_FORCE_INLINE Producer createProducer() {
    if (!operator bool()) {
      throw std::runtime_error("queue not initialized");
    }
    if (!file_.tryLock()) {
      throw std::runtime_error("can't create producer (already exists?)");
    }
    return Producer(detail::mapFile(file_));
  }

  /// Create consumer for the queue. Throws on error.
  [[nodiscard]] TURBOQ_FORCE_INLINE Consumer createConsumer() {
    if (!operator bool()) {
      throw std::runtime_error("queue not initialized");
    }
    return Consumer(detail::mapFile(file_));
  }

  /// Swap resources with other queue.
  void swap(BoundedSPMCRawQueue& that) noexcept {
    using std::swap;
    swap(file_, that.file_);
  }

  /// \see BoundedSPMCRawQueue::swap
  friend void swap(BoundedSPMCRawQueue& a, BoundedSPMCRawQueue& b) noexcept {
    a.swap(b);
  }
};

} // namespace turboq
