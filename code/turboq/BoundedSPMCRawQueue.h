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

#include <turboq/MappedRegion.h>
#include <turboq/MemorySource.h>
#include <turboq/detail/math.h>
#include <turboq/detail/memory.h>
#include <turboq/platform.h>

namespace turboq {
namespace detail {

/// SPMC queue detail
template <typename Traits>
struct BoundedSPMCRawQueueDetail {
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
  }
};

/// Implements a SPMC queue producer
template <typename Traits>
class BoundedSPMCRawQueueProducer {
private:
  using QueueDetail = BoundedSPMCRawQueueDetail<Traits>;
  using MemoryHeader = typename QueueDetail::MemoryHeader;
  using MessageHeader = typename QueueDetail::MessageHeader;

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

    if (!QueueDetail::check(content)) {
      throw std::runtime_error("invalid queue");
    }

    header_ = std::bit_cast<MemoryHeader*>(storage_.data());
    data_ = content.subspan(QueueDetail::kDataStartPos);
    producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
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
    std::size_t const alignedSize = QueueDetail::alignBufferSize(size + sizeof(MessageHeader));

    lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
    lastMessageHeader_->size = alignedSize - sizeof(MessageHeader);
    lastMessageHeader_->payloadSize = size;

    if (producerPosCache_ + alignedSize + sizeof(MessageHeader) > data_.size()) [[unlikely]] {
      lastMessageHeader_->size = QueueDetail::alignBufferSize(size);
      producerPosCache_ = 0;
      // TODO[???]:
      // lastMessageHeader_->size = detail::ceil(size, kHardwareDestructiveInterferenceSize)
    } else {
      producerPosCache_ += sizeof(MessageHeader);
    }

    lastMessageHeader_->payloadOffset = producerPosCache_;
    producerPosCache_ += lastMessageHeader_->size;

    return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
  }

  /// Make reserved buffer visible for consumers
  TURBOQ_FORCE_INLINE void commit() noexcept {
    std::atomic_ref(header_->producerPos).store(producerPosCache_, std::memory_order_release);
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

/// Implements a SPMC queue consumer
template <typename Traits>
class BoundedSPMCRawQueueConsumer {
private:
  using QueueDetail = BoundedSPMCRawQueueDetail<Traits>;
  using MemoryHeader = typename QueueDetail::MemoryHeader;
  using MessageHeader = typename QueueDetail::MessageHeader;

  MappedRegion storage_;
  std::span<std::byte> data_;
  MemoryHeader* header_ = nullptr;
  std::size_t consumerPosCache_ = 0;
  std::size_t producerPosCache_ = 0;
  MessageHeader* lastMessageHeader_ = nullptr;

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

    if (!QueueDetail::check(content)) {
      throw std::runtime_error("invalid queue");
    }

    header_ = std::bit_cast<MemoryHeader*>(content.data());
    data_ = content.subspan(QueueDetail::kDataStartPos);
    consumerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_relaxed);
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
        (producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire)) ==
            consumerPosCache_) {
      return {};
    }

    lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + consumerPosCache_);
    return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
  }

  /// Consume buffer and make buffer space available for producer
  /// pre: fetch() -> non empty buffer
  TURBOQ_FORCE_INLINE void consume() noexcept {
    consumerPosCache_ = lastMessageHeader_->payloadOffset + lastMessageHeader_->size;
  }

  /// Reset queue
  TURBOQ_FORCE_INLINE void reset() noexcept {
    consumerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_relaxed);
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
    swap(lastMessageHeader_, that.lastMessageHeader_);
  }

  /// \see BoundedSPMCRawQueueConsumer::swap
  friend void swap(BoundedSPMCRawQueueConsumer& a, BoundedSPMCRawQueueConsumer& b) noexcept {
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
class BoundedSPMCRawQueueImpl;

struct BoundedSPMCRawQueueDefaultTraits {
  static constexpr std::string_view kTag = "turboq/SPMC";
  static constexpr std::size_t kSegmentSize = kHardwareDestructiveInterferenceSize;
  static constexpr std::size_t kAlign = kHardwareDestructiveInterferenceSize;
};

using BoundedSPMCRawQueue = BoundedSPMCRawQueueImpl<BoundedSPMCRawQueueDefaultTraits>;

template <typename Traits>
class BoundedSPMCRawQueueImpl {
private:
  using QueueDetail = detail::BoundedSPMCRawQueueDetail<Traits>;
  using MemoryHeader = typename QueueDetail::MemoryHeader;
  using MessageHeader = typename QueueDetail::MessageHeader;

  File file_;

public:
  using Producer = detail::BoundedSPMCRawQueueProducer<Traits>;
  using Consumer = detail::BoundedSPMCRawQueueConsumer<Traits>;

  struct CreationOptions {
    std::size_t capacityHint;
  };

  BoundedSPMCRawQueueImpl(BoundedSPMCRawQueueImpl const&) = delete;
  BoundedSPMCRawQueueImpl& operator=(BoundedSPMCRawQueueImpl const&) = delete;
  BoundedSPMCRawQueueImpl() = default;

  BoundedSPMCRawQueueImpl(BoundedSPMCRawQueueImpl&& that) noexcept {
    swap(that);
  }

  BoundedSPMCRawQueueImpl& operator=(BoundedSPMCRawQueueImpl&& that) noexcept {
    swap(that);
    return *this;
  }

  /// Open only queue. Throws on error.
  BoundedSPMCRawQueueImpl(std::string_view name, MemorySource const& memorySource = DefaultMemorySource()) {
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
  BoundedSPMCRawQueueImpl(
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
  void swap(BoundedSPMCRawQueueImpl& that) noexcept {
    using std::swap;
    swap(file_, that.file_);
  }

  /// \see BoundedSPMCRawQueueImpl::swap
  friend void swap(BoundedSPMCRawQueueImpl& a, BoundedSPMCRawQueueImpl& b) noexcept {
    a.swap(b);
  }
};

} // namespace turboq
