// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

#include "MappedRegion.h"
#include "MemorySource.h"
#include "detail/math.h"
#include "detail/memory.h"
#include "platform.h"

namespace turboq {
namespace detail {

/// SPSC queue detail
template <typename Traits>
struct BoundedSPSCRawQueueDetail {
  /// Queue tag
  static constexpr std::string_view kTag = Traits::kTag;
  /// Segment size
  static constexpr std::size_t kSegmentSize = Traits::kSegmentSize;
  /// Alignment
  static constexpr std::size_t kAlign = Traits::kAlign;

  /// Control struct for queue buffer
  struct MemoryHeader {
    /// Placeholder for queue tag
    char tag[kTag.size()];
    /// Producer position
    alignas(kAlign) std::size_t producerPos;
    /// Consumer position
    alignas(kAlign) std::size_t consumerPos;

    static_assert(std::atomic_ref<std::size_t>::is_always_lock_free);
  };
  static_assert(std::is_trivially_copyable_v<MemoryHeader>);

  /// Control struct for message
  struct MessageHeader {
    std::size_t size;
    std::size_t payloadOffset;
    std::size_t payloadSize;
  };
  static_assert(std::is_trivially_copyable_v<MessageHeader>);

  /// Align message buffer size
  static constexpr std::size_t alignBufferSize(std::size_t value) noexcept {
    return detail::align_up(value, kSegmentSize);
  }

  /// Offset for the first message header from memory buffer start
  static constexpr std::size_t kDataStartPos = alignBufferSize(sizeof(MemoryHeader));

  /// Min buffer size
  static constexpr std::size_t kMinBufferSize = kDataStartPos + 2 * kSegmentSize;

  /// Check buffer points to valid SPMC queue region
  /// Return true on success and false otherwise.
  [[nodiscard]] static bool check(std::span<std::byte const> buffer) noexcept {
    if (buffer.size() < kMinBufferSize) {
      return false;
    }
    auto const header = std::bit_cast<MemoryHeader const*>(buffer.data());
    if (!std::equal(kTag.begin(), kTag.end(), header->tag)) {
      return false;
    }
    return true;
  }

  /// Init queue memory header
  static void init(std::span<std::byte> buffer) noexcept {
    auto header = std::bit_cast<MemoryHeader*>(buffer.data());
    std::copy(kTag.begin(), kTag.end(), header->tag);
    std::atomic_ref(header->producerPos).store(0, std::memory_order_relaxed);
    std::atomic_ref(header->consumerPos).store(0, std::memory_order_relaxed);
  }
};

/// Implements a SPSC queue producer
template <typename Traits>
class BoundedSPSCRawQueueProducer {
private:
  using QueueDetail = BoundedSPSCRawQueueDetail<Traits>;
  using MemoryHeader = typename QueueDetail::MemoryHeader;
  using MessageHeader = typename QueueDetail::MessageHeader;

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

    if (!QueueDetail::check(content)) {
      throw std::runtime_error("invalid queue");
    }

    header_ = std::bit_cast<MemoryHeader*>(content.data());
    data_ = content.subspan(QueueDetail::kDataStartPos);
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
    std::size_t const alignedSize = QueueDetail::alignBufferSize(size + sizeof(MessageHeader));

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
      std::size_t const alignedSize2 = QueueDetail::alignBufferSize(size);
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
  TURBOQ_FORCE_INLINE void commit(std::size_t size) {
    // TODO: new size could be greater previous but less than lastMessageHeader_->size
    if (size <= lastMessageHeader_->payloadSize) [[likely]] {
      lastMessageHeader_->payloadSize = size;
    } else {
      throw std::runtime_error("new commit size greater previously requested size");
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

/// Implements a SPSC queue consumer
template <typename Traits>
class BoundedSPSCRawQueueConsumer {
private:
  using QueueDetail = BoundedSPSCRawQueueDetail<Traits>;
  using MemoryHeader = typename QueueDetail::MemoryHeader;
  using MessageHeader = typename QueueDetail::MessageHeader;

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

    if (!QueueDetail::check(content)) {
      throw std::runtime_error("invalid queue");
    }

    header_ = std::bit_cast<MemoryHeader*>(content.data());
    data_ = content.subspan(QueueDetail::kDataStartPos);
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

} // namespace detail

/// Queue layout:
/// s               e   s                      e  s                    e
/// +---------------+---+--------+-------------+--+--------+-----------+-----+--------
/// | MemoryHeader  |xxx| Header | Payload     |xx| Header |  Payload  |xxxxx|uuuuuuuu ...
/// +---------------+---+--------+-------------+--+--------+-----------+-----+--------
/// s   - start
/// e   - end
/// xxx - padding bytes
/// uuu - unused bytes
template <typename Traits>
class BoundedSPSCRawQueueImpl;

struct BoundedSPSCRawQueueDefaultTraits {
  static constexpr std::string_view kTag = "turboq/SPSC";
  static constexpr std::size_t kSegmentSize = kHardwareDestructiveInterferenceSize;
  static constexpr std::size_t kAlign = kHardwareDestructiveInterferenceSize;
};

using BoundedSPSCRawQueue = BoundedSPSCRawQueueImpl<BoundedSPSCRawQueueDefaultTraits>;

template <typename Traits>
class BoundedSPSCRawQueueImpl {
private:
  using QueueDetail = detail::BoundedSPSCRawQueueDetail<Traits>;
  using MemoryHeader = typename QueueDetail::MemoryHeader;
  using MessageHeader = typename QueueDetail::MessageHeader;

  File file_;

public:
  using Producer = detail::BoundedSPSCRawQueueProducer<Traits>;
  using Consumer = detail::BoundedSPSCRawQueueConsumer<Traits>;

  struct CreationOptions {
    std::size_t capacityHint;
  };

  BoundedSPSCRawQueueImpl(BoundedSPSCRawQueueImpl const&) = delete;
  BoundedSPSCRawQueueImpl& operator=(BoundedSPSCRawQueueImpl const&) = delete;
  BoundedSPSCRawQueueImpl() = default;

  BoundedSPSCRawQueueImpl(BoundedSPSCRawQueueImpl&& that) noexcept {
    swap(that);
  }

  BoundedSPSCRawQueueImpl& operator=(BoundedSPSCRawQueueImpl&& that) noexcept {
    swap(that);
    return *this;
  }

  /// Open only queue. Throws on error.
  BoundedSPSCRawQueueImpl(std::string_view name, MemorySource const& memorySource = DefaultMemorySource()) {
    auto result = memorySource.open(name, MemorySource::OpenOnly);
    if (!result) {
      throw std::runtime_error("failed to open memory source");
    }
    std::size_t pageSize;
    std::tie(file_, pageSize) = std::move(result).value();

    if (auto storage = detail::mapFile(file_); !QueueDetail::check(storage.content())) {
      throw std::runtime_error("failed to open queue (invalid)");
    }
  }

  /// Open or create queue. Throws on error.
  BoundedSPSCRawQueueImpl(
      std::string_view name, CreationOptions const& options, MemorySource const& memorySource = DefaultMemorySource()) {
    auto result = memorySource.open(name, MemorySource::OpenOrCreate);
    if (!result) {
      throw std::runtime_error("failed to open memory source");
    }

    std::size_t pageSize;
    std::tie(file_, pageSize) = std::move(result).value();

    // round-up requested size to page size
    std::size_t const capacity = detail::align_up(options.capacityHint, pageSize);

    // init queue or check queue's options is the same as requested
    if (auto const fileSize = file_.getFileSize(); fileSize != 0) {
      if (fileSize != capacity) {
        throw std::runtime_error("size mismatch");
      }
      if (auto storage = detail::mapFile(file_); !QueueDetail::check(storage.content())) {
        throw std::runtime_error("failed to open queue (invalid)");
      }
    } else {
      file_.truncate(capacity);
      QueueDetail::init(detail::mapFile(file_, capacity).content());
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
    return Producer(detail::mapFile(file_));
  }

  /// Create consumer for the queue. Throws on error.
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
  void swap(BoundedSPSCRawQueueImpl& that) noexcept {
    using std::swap;
    swap(file_, that.file_);
  }

  /// \see BoundedSPSCRawQueueImpl::swap
  friend void swap(BoundedSPSCRawQueueImpl& a, BoundedSPSCRawQueueImpl& b) noexcept {
    a.swap(b);
  }
};

} // namespace turboq
