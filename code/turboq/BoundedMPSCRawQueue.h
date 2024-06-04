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

#include <fmt/format.h>

#include <turboq/MappedRegion.h>
#include <turboq/MemorySource.h>
#include <turboq/detail/math.h>
#include <turboq/detail/mmap.h>
#include <turboq/platform.h>

namespace turboq {
namespace detail {

/// MPSC queue detail
template <typename Traits>
struct BoundedMPSCRawQueueDetail {
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
    /// Max message size
    std::size_t maxMessageSize;
    /// Queue length
    std::size_t length;
    /// Consumer position
    alignas(kAlign) std::size_t consumerPos;
    /// Producer position
    alignas(kAlign) std::size_t producerPos;

    static_assert(std::atomic_ref<std::size_t>::is_always_lock_free);
  };

  /// Control struct for message
  struct MessageHeader {
    std::size_t payloadSize;
  };

  /// Control struct for commit state
  struct StateHeader {
    alignas(kAlign) bool commited;

    static_assert(std::atomic_ref<bool>::is_always_lock_free);
  };

  /// Align message buffer size
  static constexpr std::size_t alignBufferSize(std::size_t value) noexcept {
    return detail::align_up(value, kSegmentSize);
  }

  /// Offset for the first message header from memory buffer start
  static constexpr std::size_t kDataStartPos = alignBufferSize(sizeof(MemoryHeader));

  /// Check buffer points to valid SPMC queue region
  /// Return true on success and false otherwise.
  [[nodiscard]] static bool check(std::span<std::byte const> buffer) noexcept {
    auto const header = std::bit_cast<MemoryHeader const*>(buffer.data());
    if (header->maxMessageSize == 0 || header->length == 0) {
      return false;
    }
    if (!std::equal(kTag.begin(), kTag.end(), header->tag)) {
      return false;
    }
    return true;
  }

  /// Init queue memory header
  static void init(std::span<std::byte> buffer, std::size_t maxMessageSize, std::size_t length) noexcept {
    auto header = std::bit_cast<MemoryHeader*>(buffer.data());
    std::copy(kTag.begin(), kTag.end(), header->tag);
    header->maxMessageSize = maxMessageSize;
    header->length = length;
  }
};

/// Implements a MPSC queue producer
template <typename Traits>
class BoundedMPSCRawQueueProducer {
private:
  using QueueDetail = BoundedMPSCRawQueueDetail<Traits>;
  using MemoryHeader = typename QueueDetail::MemoryHeader;
  using MessageHeader = typename QueueDetail::MessageHeader;
  using StateHeader = typename QueueDetail::StateHeader;

  MappedRegion storage_;
  MemoryHeader* header_ = nullptr;
  std::span<std::byte> data_;
  std::span<StateHeader> commitStates_;
  std::size_t producerPosCache_ = 0;
  std::size_t consumerPosCache_ = 0;

public:
  BoundedMPSCRawQueueProducer() = default;
  ~BoundedMPSCRawQueueProducer() = default;

  BoundedMPSCRawQueueProducer(BoundedMPSCRawQueueProducer&& that) noexcept {
    swap(that);
  }

  BoundedMPSCRawQueueProducer& operator=(BoundedMPSCRawQueueProducer&& that) noexcept {
    swap(that);
    return *this;
  }

  BoundedMPSCRawQueueProducer(MappedRegion&& storage) : storage_(std::move(storage)) {
    auto content = storage_.content();

    if (!QueueDetail::check(content)) {
      throw std::runtime_error("invalid queue");
    }

    header_ = std::bit_cast<MemoryHeader*>(storage_.data());
    std::size_t offset = QueueDetail::kDataStartPos;

    data_ = std::span<std::byte>(storage_.data() + offset, header_->maxMessageSize * header_->length);
    offset += (header_->maxMessageSize * header_->length);

    commitStates_ = std::span<StateHeader>(std::bit_cast<StateHeader*>(storage_.data() + offset), header_->length);
    consumerPosCache_ = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);
  }

  /// Return true on initialized
  [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
    return static_cast<bool>(storage_);
  }

  /// Return queue max message size
  [[nodiscard]] TURBOQ_FORCE_INLINE std::size_t maxMessageSize() const noexcept {
    if (operator bool()) [[likely]] {
      return header_->maxMessageSize;
    }
    return 0;
  }

  /// Return queue length (max messages count)
  [[nodiscard]] TURBOQ_FORCE_INLINE std::size_t length() const noexcept {
    if (operator bool()) [[likely]] {
      return header_->length;
    }
    return 0;
  }

  /// Reserve contiguous space for writing without making it visible to the consumers
  /// \throw std::runtime_error in case of requested size greater max message size
  [[nodiscard]] TURBOQ_FORCE_INLINE std::span<std::byte> prepare(std::size_t size) {
    std::size_t const totalSize = size + sizeof(MessageHeader);
    if (totalSize > header_->maxMessageSize) [[unlikely]] {
      throw std::runtime_error(
          fmt::format("buffer exceed max message size ({} > {})", totalSize, header_->maxMessageSize));
    }

    std::size_t currentProducerPos = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
    if (currentProducerPos - consumerPosCache_ >= header_->length) [[unlikely]] {
      consumerPosCache_ = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);
      if (currentProducerPos - consumerPosCache_ >= header_->length) [[unlikely]] {
        return {};
      }
    }

    while (!std::atomic_ref(header_->producerPos)
                .compare_exchange_weak(currentProducerPos, currentProducerPos + 1, std::memory_order_release,
                    std::memory_order_relaxed)) [[unlikely]] {
      if (currentProducerPos - consumerPosCache_ >= header_->length) [[unlikely]] {
        return {};
      }
    }

    producerPosCache_ = currentProducerPos & (header_->length - 1);
    std::byte* content = data_.data() + producerPosCache_ * header_->maxMessageSize;
    std::bit_cast<MessageHeader*>(content)->payloadSize = size;

    return {content + sizeof(MessageHeader), size};
  }

  /// Make reserved buffer visible for consumers
  TURBOQ_FORCE_INLINE void commit() noexcept {
    std::atomic_ref(commitStates_[producerPosCache_].commited).store(true, std::memory_order_release);
  }

  /// \overload
  TURBOQ_FORCE_INLINE void commit(std::size_t size) noexcept {
    auto header = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_ * header_->maxMessageSize);
    if (size <= header->payloadSize) [[likely]] {
      header->payloadSize = size;
    } else {
      assert(false);
    }
    commit();
  }

  /// Swap resources with other producer
  void swap(BoundedMPSCRawQueueProducer& that) noexcept {
    using std::swap;
    swap(storage_, that.storage_);
    swap(header_, that.header_);
    swap(data_, that.data_);
    swap(commitStates_, that.commitStates_);
    swap(producerPosCache_, that.producerPosCache_);
    swap(consumerPosCache_, that.consumerPosCache_);
  }

  /// \see BoundedMPSCRawQueueProducer::swap
  friend void swap(BoundedMPSCRawQueueProducer& a, BoundedMPSCRawQueueProducer& b) noexcept {
    a.swap(b);
  }
};

/// Implements a MPSC queue consumer
template <typename Traits>
class BoundedMPSCRawQueueConsumer {
private:
  using QueueDetail = BoundedMPSCRawQueueDetail<Traits>;
  using MemoryHeader = typename QueueDetail::MemoryHeader;
  using MessageHeader = typename QueueDetail::MessageHeader;
  using StateHeader = typename QueueDetail::StateHeader;

  MappedRegion storage_;
  MemoryHeader* header_ = nullptr;
  std::span<std::byte> data_;
  std::span<StateHeader> commitStates_;
  std::size_t producerPosCache_ = 0;
  std::size_t consumerPosCache_ = 0;
  MessageHeader* lastMessageHeader_ = nullptr;
  StateHeader* lastCommitState_ = nullptr;

public:
  BoundedMPSCRawQueueConsumer() = default;
  ~BoundedMPSCRawQueueConsumer() = default;

  BoundedMPSCRawQueueConsumer(BoundedMPSCRawQueueConsumer&& that) noexcept {
    swap(that);
  }

  BoundedMPSCRawQueueConsumer& operator=(BoundedMPSCRawQueueConsumer&& that) noexcept {
    swap(that);
    return *this;
  }

  BoundedMPSCRawQueueConsumer(MappedRegion&& storage) : storage_(std::move(storage)) {
    auto content = storage_.content();

    if (!QueueDetail::check(content)) {
      throw std::runtime_error("invalid queue");
    }

    header_ = std::bit_cast<MemoryHeader*>(storage_.data());

    std::size_t offset = QueueDetail::kDataStartPos;
    data_ = std::span<std::byte>(content.data() + offset, header_->maxMessageSize * header_->length);

    offset += header_->maxMessageSize * header_->length;
    commitStates_ = std::span<StateHeader>(std::bit_cast<StateHeader*>(storage_.data() + offset), header_->length);

    producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
    consumerPosCache_ = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);
  }

  /// Return true on initialized
  [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
    return static_cast<bool>(storage_);
  }

  /// Return queue max message size
  [[nodiscard]] TURBOQ_FORCE_INLINE std::size_t maxMessageSize() const noexcept {
    if (operator bool()) [[likely]] {
      return header_->maxMessageSize;
    }
    return 0;
  }

  /// Return queue length (max messages count)
  [[nodiscard]] TURBOQ_FORCE_INLINE std::size_t length() const noexcept {
    if (operator bool()) [[likely]] {
      return header_->length;
    }
    return 0;
  }

  /// Get next buffer for reading. Return empty buffer in case of no data.
  [[nodiscard]] TURBOQ_FORCE_INLINE std::span<std::byte const> fetch() noexcept {
    if ((consumerPosCache_ == producerPosCache_ &&
            (producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire)) ==
                consumerPosCache_)) [[unlikely]] {
      return {};
    }

    std::size_t const consumerPos = consumerPosCache_ & (header_->length - 1);

    lastCommitState_ = &commitStates_[consumerPos];
    if (!std::atomic_ref(lastCommitState_->commited).load(std::memory_order_acquire)) [[unlikely]] {
      return {};
    }

    lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + consumerPos * header_->maxMessageSize);
    return {std::bit_cast<std::byte*>(lastMessageHeader_ + 1), lastMessageHeader_->payloadSize};
  }

  /// Consume front buffer and make buffer available for producer
  /// pre: fetch() -> non empty buffer
  TURBOQ_FORCE_INLINE void consume() noexcept {
    consumerPosCache_++;
    std::atomic_ref(lastCommitState_->commited).store(false, std::memory_order_release);
    std::atomic_ref(header_->consumerPos).store(consumerPosCache_, std::memory_order_release);
  }

  /// Reset queue
  TURBOQ_FORCE_INLINE void reset() noexcept {
    while (consumerPosCache_ != producerPosCache_) {
      // Drop message.
      std::size_t const consumerPos = consumerPosCache_ & (header_->length - 1);
      lastCommitState_ = &commitStates_[consumerPos];
      std::atomic_ref(lastCommitState_->commited).store(false, std::memory_order_release);
      consumerPosCache_++;
    }
    std::atomic_ref(header_->consumerPos).store(consumerPosCache_, std::memory_order_release);
  }

  /// Swap resources with other object
  void swap(BoundedMPSCRawQueueConsumer& that) noexcept {
    using std::swap;
    swap(storage_, that.storage_);
    swap(header_, that.header_);
    swap(data_, that.data_);
    swap(commitStates_, that.commitStates_);
    swap(producerPosCache_, that.producerPosCache_);
    swap(consumerPosCache_, that.consumerPosCache_);
    swap(lastMessageHeader_, that.lastMessageHeader_);
    swap(lastCommitState_, that.lastCommitState_);
  }

  /// \see BoundedMPSCRawQueueConsumer::swap
  friend void swap(BoundedMPSCRawQueueConsumer& a, BoundedMPSCRawQueueConsumer& b) noexcept {
    a.swap(b);
  }
};

} // namespace detail

template <typename Traits>
class BoundedMPSCRawQueueImpl;

struct BoundedMPSCRawQueueDefaultTraits {
  static constexpr std::string_view kTag = "turboq/MPSC";
  static constexpr std::size_t kSegmentSize = kHardwareDestructiveInterferenceSize;
  static constexpr std::size_t kAlign = kHardwareDestructiveInterferenceSize;
};

using BoundedMPSCRawQueue = BoundedMPSCRawQueueImpl<BoundedMPSCRawQueueDefaultTraits>;

template <typename Traits>
class BoundedMPSCRawQueueImpl {
private:
  using QueueDetail = detail::BoundedMPSCRawQueueDetail<Traits>;
  using MemoryHeader = typename QueueDetail::MemoryHeader;
  using MessageHeader = typename QueueDetail::MessageHeader;
  using StateHeader = typename QueueDetail::StateHeader;

  File file_;

public:
  using Producer = detail::BoundedMPSCRawQueueProducer<Traits>;
  using Consumer = detail::BoundedMPSCRawQueueConsumer<Traits>;

  struct CreationOptions {
    std::size_t maxMessageSizeHint;
    std::size_t lengthHint;
  };

  BoundedMPSCRawQueueImpl(BoundedMPSCRawQueueImpl const&) = delete;
  BoundedMPSCRawQueueImpl& operator=(BoundedMPSCRawQueueImpl const&) = delete;
  BoundedMPSCRawQueueImpl() = default;

  BoundedMPSCRawQueueImpl(BoundedMPSCRawQueueImpl&& that) noexcept {
    swap(that);
  }

  BoundedMPSCRawQueueImpl& operator=(BoundedMPSCRawQueueImpl&& that) noexcept {
    swap(that);
    return *this;
  }

  /// Open only queue. Throws on error.
  BoundedMPSCRawQueueImpl(std::string_view name, MemorySource const& memorySource = DefaultMemorySource()) {
    auto result = memorySource.open(name, MemorySource::OpenOnly);
    if (!result) {
      throw std::runtime_error("failed to open memory source");
    }

    std::size_t pageSize;
    std::tie(file_, pageSize) = std::move(result).assume_value();

    if (auto storage = detail::mapFile(file_); !QueueDetail::check(storage.content())) {
      throw std::runtime_error("failed to open queue (invalid)");
    }
  }

  /// Open or create queue. Throws on error.
  BoundedMPSCRawQueueImpl(
      std::string_view name, CreationOptions const& options, MemorySource const& memorySource = DefaultMemorySource()) {
    if (options.maxMessageSizeHint == 0) {
      throw std::runtime_error("invalid argument (max message size)");
    }
    if (options.lengthHint == 0) {
      throw std::runtime_error("invalid argument (length)");
    }
    auto result = memorySource.open(name, MemorySource::OpenOrCreate);
    if (!result) {
      throw std::runtime_error("failed to open memory source");
    }

    std::size_t pageSize;
    std::tie(file_, pageSize) = std::move(result).assume_value();

    auto const maxMessageSize = QueueDetail::alignBufferSize(options.maxMessageSizeHint + sizeof(MessageHeader));
    auto const length = detail::upper_pow_2(options.lengthHint);
    auto const capacityHint =
        QueueDetail::alignBufferSize(sizeof(MemoryHeader)) + maxMessageSize * length + sizeof(StateHeader) * length;
    // round-up requested size to page size
    auto const capacity = detail::align_up(capacityHint, pageSize);

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
      QueueDetail::init(detail::mapFile(file_, capacity).content(), maxMessageSize, length);
    }
  }

  /// Return true on queue intialized.
  [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
    return static_cast<bool>(file_);
  }

  /// Create producer for the queue. Throws on error.
  [[nodiscard]] TURBOQ_FORCE_INLINE Producer createProducer() {
    if (!operator bool()) {
      throw std::runtime_error("queue in not initialized");
    }
    return Producer(detail::mapFile(file_));
  }

  /// Create consumer for the queue. Throws on error.
  [[nodiscard]] TURBOQ_FORCE_INLINE Consumer createConsumer() {
    if (!operator bool()) {
      throw std::runtime_error("queue in not initialized");
    }
    if (!file_.tryLock()) {
      throw std::runtime_error("can't create consumer (already exists?)");
    }
    return Consumer(detail::mapFile(file_));
  }

  /// Swap resources with other queue.
  void swap(BoundedMPSCRawQueueImpl& that) noexcept {
    using std::swap;
    swap(file_, that.file_);
  }

  /// \see BoundedMPSCRawQueueImpl::swap
  friend void swap(BoundedMPSCRawQueueImpl& a, BoundedMPSCRawQueueImpl& b) noexcept {
    a.swap(b);
  }
};

} // namespace turboq
