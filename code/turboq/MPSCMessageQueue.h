// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "Error.h"
#include "MappedRegion.h"
#include "Math.h"
#include "MemorySource.h"
#include "Platform.h"
#include "Utils.h"

namespace turboq {
namespace detail {

template <typename Options>
struct MPSCMessageQueueLayout {
    static constexpr std::string_view kTag = Options::tag;

    struct MemoryHeader {
        char tag[kTag.size()];
        std::size_t slotSize;
        std::size_t length;
        alignas(kCacheLineSize) std::size_t consumerPos;
        alignas(kCacheLineSize) std::size_t producerPos;
    };

    struct SlotHeader {
        alignas(kCacheLineSize) bool committed;
        std::size_t payloadSize;
    };

    static_assert(std::atomic_ref<std::size_t>::is_always_lock_free);
    static_assert(std::is_trivially_copyable_v<MemoryHeader>);
    static_assert(std::is_trivially_copyable_v<SlotHeader>);

    /// Align size to cache line boundary
    /// Used to prevent false sharing between producer and consumer
    static constexpr auto makeCacheLineAligned(std::size_t size) noexcept -> std::size_t {
        return alignUp(size, kCacheLineSize);
    }

    /// Size of the memory header buffer, aligned to cache line
    static constexpr auto kMemoryHeaderBufferSize = makeCacheLineAligned(sizeof(MemoryHeader));

    /// Size of the slot header buffer, aligned to cache line
    static constexpr auto kSlotHeaderBufferSize = makeCacheLineAligned(sizeof(SlotHeader));
};

/// MPSC queue producer
template <typename Options>
class MPSCMessageQueueProducerImpl {
private:
    using Details = MPSCMessageQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using SlotHeader = typename Details::SlotHeader;

    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::span<std::byte> slots_;
    std::size_t producerPosCache_{0};
    std::size_t consumerPosCache_{0};

public:
    MPSCMessageQueueProducerImpl() = default;
    ~MPSCMessageQueueProducerImpl() = default;

    MPSCMessageQueueProducerImpl(MPSCMessageQueueProducerImpl&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, slots_{std::move(other.slots_)},
          producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          consumerPosCache_{std::exchange(other.consumerPosCache_, 0)} {}

    MPSCMessageQueueProducerImpl& operator=(MPSCMessageQueueProducerImpl&& other) noexcept {
        if (this != &other) {
            this->~MPSCMessageQueueProducerImpl();
            new (this) MPSCMessageQueueProducerImpl{std::move(other)};
        }
        return *this;
    }

    MPSCMessageQueueProducerImpl(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        assert(storage_);

        if (auto const rc = storage_.advise(Advice::Sequential); !rc) {
            std::fprintf(stderr, "WARNING: advise failed: %s\n", rc.error().message().c_str());
        }

        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(storage_.data());

        std::size_t offset = Details::kMemoryHeaderBufferSize;
        data_ = content.subspan(offset, header_->slotSize * header_->length);

        offset += header_->slotSize * header_->length;
        slots_ = {content.data() + offset, header_->length * Details::kSlotHeaderBufferSize};

        consumerPosCache_ = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);
    }

    /// Return true on initialized
    [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
        return static_cast<bool>(storage_);
    }

    /// Return queue slot size
    [[nodiscard]] TURBOQ_FORCE_INLINE auto slotSize() const noexcept -> std::size_t {
        return header_ ? header_->slotSize : 0;
    }

    /// Return queue length (total slots)
    [[nodiscard]] TURBOQ_FORCE_INLINE auto length() const noexcept -> std::size_t {
        return header_ ? header_->length : 0;
    }

    /// Reserve contiguous space for writing without making it visible to the consumers, throws on size exceed slot max
    /// message size
    [[nodiscard]] TURBOQ_FORCE_INLINE auto prepare(std::size_t size) -> std::span<std::byte> {
        if (size > header_->slotSize) [[unlikely]] {
            throw std::system_error{makeErrorCode(Error::MessageSizeExceedSlotSize), "message size exceed slot size"};
        }

        auto currentProducerPos = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
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
            cpuRelax();
        }

        producerPosCache_ = currentProducerPos & (header_->length - 1);
        auto const dataPtr = data_.data() + producerPosCache_ * header_->slotSize;
        auto const slotPtr = slots_.data() + producerPosCache_ * Details::kSlotHeaderBufferSize;
        std::bit_cast<SlotHeader*>(slotPtr)->payloadSize = size;

        return {dataPtr, size};
    }

    /// Make reserved buffer visible for consumers
    TURBOQ_FORCE_INLINE void commit() noexcept {
        auto const slotPtr = slots_.data() + producerPosCache_ * Details::kSlotHeaderBufferSize;
        std::atomic_ref(std::bit_cast<SlotHeader*>(slotPtr)->committed).store(true, std::memory_order_release);
    }

    /// \overload
    TURBOQ_FORCE_INLINE void commit(std::size_t size) noexcept {
        auto const slotPtr = slots_.data() + producerPosCache_ * Details::kSlotHeaderBufferSize;
        auto const slotHeaderPtr = std::bit_cast<SlotHeader*>(slotPtr);
        if (size <= slotHeaderPtr->payloadSize) {
            slotHeaderPtr->payloadSize = size;
        } else {
            assert(false);
        }
        this->commit();
    }
};

/// MPSC queue consumer
template <typename Options>
class MPSCMessageQueueConsumerImpl {
private:
    using Details = MPSCMessageQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using SlotHeader = typename Details::SlotHeader;

    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::span<std::byte> slots_;
    std::size_t producerPosCache_{0};
    std::size_t consumerPosCache_{0};
    SlotHeader* lastSlotHeader_{nullptr};

public:
    MPSCMessageQueueConsumerImpl() = default;
    ~MPSCMessageQueueConsumerImpl() = default;

    MPSCMessageQueueConsumerImpl(MPSCMessageQueueConsumerImpl&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, slots_{std::move(other.slots_)},
          producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          consumerPosCache_{std::exchange(other.consumerPosCache_, 0)},
          lastSlotHeader_{std::exchange(other.lastSlotHeader_, nullptr)} {}

    MPSCMessageQueueConsumerImpl& operator=(MPSCMessageQueueConsumerImpl&& other) noexcept {
        if (this != &other) {
            this->~MPSCMessageQueueConsumerImpl();
            new (this) MPSCMessageQueueConsumerImpl{std::move(other)};
        }
        return *this;
    }

    MPSCMessageQueueConsumerImpl(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        assert(storage_);

        if (auto const rc = storage_.advise(Advice::Sequential); !rc) {
            std::fprintf(stderr, "WARNING: advise failed: %s\n", rc.error().message().c_str());
        }

        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(storage_.data());

        std::size_t offset = Details::kMemoryHeaderBufferSize;
        data_ = content.subspan(offset, header_->slotSize * header_->length);

        offset += header_->slotSize * header_->length;
        slots_ = {content.data() + offset, header_->length * Details::kSlotHeaderBufferSize};

        producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
        consumerPosCache_ = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);

        assert((reinterpret_cast<uintptr_t>(data_.data()) & (kCacheLineSize - 1)) == 0);
        assert((reinterpret_cast<uintptr_t>(slots_.data()) & (kCacheLineSize - 1)) == 0);
    }

    /// Return true on initialized
    [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
        return static_cast<bool>(storage_);
    }

    /// Return queue max message size
    [[nodiscard]] TURBOQ_FORCE_INLINE auto slotSize() const noexcept -> std::size_t {
        return header_ ? header_->slotSize : 0;
    }

    /// Return queue length (max messages count)
    [[nodiscard]] TURBOQ_FORCE_INLINE auto length() const noexcept -> std::size_t {
        return header_ ? header_->length : 0;
    }

    /// Get next buffer for reading. Return empty buffer in case of no data.
    [[nodiscard]] TURBOQ_FORCE_INLINE auto fetch() noexcept -> std::span<std::byte const> {
        if (consumerPosCache_ == producerPosCache_ &&
            (producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire)) ==
                consumerPosCache_) [[unlikely]] {
            return {};
        }

        auto const consumerPos = consumerPosCache_ & (header_->length - 1);

        auto const slotPtr = slots_.data() + consumerPos * Details::kSlotHeaderBufferSize;
        lastSlotHeader_ = std::bit_cast<SlotHeader*>(slotPtr);
        assert((reinterpret_cast<uintptr_t>(lastSlotHeader_) & (kCacheLineSize - 1)) == 0);

        if (!std::atomic_ref(lastSlotHeader_->committed).load(std::memory_order_acquire)) [[unlikely]] {
            return {};
        }

        auto const dataPtr = data_.data() + consumerPos * header_->slotSize;
        assert((reinterpret_cast<uintptr_t>(dataPtr) & (kCacheLineSize - 1)) == 0);

        return {dataPtr, lastSlotHeader_->payloadSize};
    }

    /// Consume buffer and make buffer space available for producer
    /// pre: fetch() -> non empty buffer
    TURBOQ_FORCE_INLINE void consume() noexcept {
        consumerPosCache_++;
        std::atomic_ref(lastSlotHeader_->committed).store(false, std::memory_order_release);
        std::atomic_ref(header_->consumerPos).store(consumerPosCache_, std::memory_order_release);
    }

    /// Reset queue
    TURBOQ_FORCE_INLINE void reset() noexcept {
        while (consumerPosCache_ != producerPosCache_) {
            // Drop message
            std::size_t const consumerPos = consumerPosCache_ & (header_->length - 1);
            auto const slotPtr = slots_.data() + consumerPos * Details::kSlotHeaderBufferSize;
            lastSlotHeader_ = std::bit_cast<SlotHeader*>(slotPtr);
            std::atomic_ref(lastSlotHeader_->committed).store(false, std::memory_order_release);
            consumerPosCache_++;
        }
        std::atomic_ref(header_->consumerPos).store(consumerPosCache_, std::memory_order_release);
    }
};

/// Memory layout:
///
///    MemoryHeader                      data_ (length fixed-size slots)                commitStates_
///   +------------------------+----------+----------+----------+     +----------+----+----+-----+-----+
///   | tag | slotSize | length| Slot 0   | Slot 1   | Slot 2   | ... | Slot N-1 |  SlotHeader[0..N-1] |
///   | consumerPos|producerPos|          |          |          |     |          |  (1 cache line each)|
///   +------------------------+----------+----------+----------+     +----------+----+----+-----+-----+
///
/// Unlike SPSC/MulticastQueue, this is a *fixed-size circular array* of `length` slots (always a
/// power of two), not a variable-size byte ring: slot index = producerPos & (length - 1), so a
/// given slot always lives at the same byte offset no matter how many times the ring has wrapped.
/// Each slot is laid out as:
///
///   +--------+-------------------------------------------------+
///   | Header | Payload (up to slotSize - sizeof(Header) bytes) |
///   +--------+-------------------------------------------------+
///
/// commitStates_ is a *separate* array living after all the slots, one SlotHeader per slot, each
/// padded out to its own cache line. A producer reserves a slot via compare_exchange on
/// producerPos, writes the message into it, then flips commitStates_[slot].committed = true --
/// kept apart from the slot data so a consumer can poll "is slot N ready yet?" without touching
/// (and dirtying the cache line of) the payload itself. Keeping reservation (producerPos) and
/// publication (committed) as two separate steps is also what lets several producers write to
/// different slots concurrently: each one only ever owns the exact slot it CAS'd for.

template <typename Options>
class MPSCMessageQueueImpl {
private:
    using Details = MPSCMessageQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using SlotHeader = typename Details::SlotHeader;

    File file_;

    MPSCMessageQueueImpl(File file) noexcept : file_{std::move(file)} {}

public:
    using Producer = MPSCMessageQueueProducerImpl<Options>;
    using Consumer = MPSCMessageQueueConsumerImpl<Options>;

    struct CreationOptions {
        std::size_t slotSizeHint;
        std::size_t lengthHint;
    };

    MPSCMessageQueueImpl(MPSCMessageQueueImpl const&) = delete;
    MPSCMessageQueueImpl& operator=(MPSCMessageQueueImpl const&) = delete;
    MPSCMessageQueueImpl() = default;

    MPSCMessageQueueImpl(MPSCMessageQueueImpl&& other) noexcept : file_{std::move(other.file_)} {}

    MPSCMessageQueueImpl& operator=(MPSCMessageQueueImpl&& other) noexcept {
        if (this != &other) {
            this->~MPSCMessageQueueImpl();
            new (this) MPSCMessageQueueImpl{std::move(other)};
        }
        return *this;
    }

    /// Construct mpsc queue (open or create), throws std::runtime_error on error
    MPSCMessageQueueImpl(std::string_view name, CreationOptions const& options,
        MemorySource const& memorySource = DefaultMemorySource{}) {
        if (options.slotSizeHint == 0) {
            throw std::system_error{
                makeErrorCode(Error::InvalidCreationOptions), "invalid max message size hint value"};
        }
        if (options.lengthHint == 0) {
            throw std::system_error{makeErrorCode(Error::InvalidCreationOptions), "invalid length hint value"};
        }

        auto openMemorySourceResult = memorySource.open(name, MemorySource::OpenOrCreate);
        if (!openMemorySourceResult) {
            throw std::system_error{openMemorySourceResult.error(), "failed to open memory source"};
        }

        auto [file, pageSize] = std::move(openMemorySourceResult).value();
        assert(pageSize > 0 && ((pageSize & (kCacheLineSize - 1)) == 0));

        auto getFileSizeResult = file.tryGetFileSize();
        if (!getFileSizeResult) {
            throw std::system_error{getFileSizeResult.error(), "failed to get queue file size"};
        }

        auto const fileSize = getFileSizeResult.value();

        // calculate slot exactly size
        auto const slotSize = Details::makeCacheLineAligned(options.slotSizeHint);
        auto const length = upperPow2(options.lengthHint);
        auto const capacityHint =
            Details::kMemoryHeaderBufferSize + slotSize * length + Details::kSlotHeaderBufferSize * length;
        // round-up requested size to page size
        auto const capacity = alignUp(capacityHint, pageSize);

        if (fileSize == 0) {
            // init queue on created
            auto const truncateResult = file.tryTruncate(capacity);
            if (!truncateResult) {
                throw std::system_error{truncateResult.error(), "failed to truncate queue file"};
            }
        } else if (capacity != fileSize) {
            // check file size on opened
            throw std::system_error{makeErrorCode(Error::SizeMismatch), "queue unexpected capacity"};
        }

        auto mapFileResult = MappedRegion::makeMappedRegion(file, pageSize);
        if (!mapFileResult) {
            throw std::system_error{mapFileResult.error(), "failed to map queue file into memory"};
        }

        auto memory = std::move(mapFileResult).value();
        auto buffer = memory.content();

        if (fileSize == 0) {
            // init queue internals
            auto header = std::bit_cast<MemoryHeader*>(buffer.data());
            std::ranges::copy(MPSCMessageQueueLayout<Options>::kTag, header->tag);
            header->slotSize = slotSize;
            header->length = length;
            std::atomic_ref(header->producerPos).store(0, std::memory_order_relaxed);
            std::atomic_ref(header->consumerPos).store(0, std::memory_order_relaxed);

            // XXX: init slots commited to false?
        }

        auto header = std::bit_cast<MemoryHeader const*>(buffer.data());
        if (!std::ranges::equal(MPSCMessageQueueLayout<Options>::kTag, header->tag)) {
            throw std::system_error{makeErrorCode(Error::TagMismatch), "unexpected queue tag value"};
        }

        file_ = std::move(file);
    }

    /// Construct multicast queue (open only), throws std::runtime_error on error
    MPSCMessageQueueImpl(std::string_view name, MemorySource const& memorySource = DefaultMemorySource{}) {
        auto openMemorySourceResult = memorySource.open(name, MemorySource::OpenOnly);
        if (!openMemorySourceResult) {
            throw std::system_error{openMemorySourceResult.error(), "failed to open memory source"};
        }

        auto [file, pageSize] = std::move(openMemorySourceResult).value();

        auto getFileSizeResult = file.tryGetFileSize();
        if (!getFileSizeResult) {
            throw std::system_error{getFileSizeResult.error(), "failed to get queue file size"};
        }
        if (getFileSizeResult.value() < Details::kMemoryHeaderBufferSize) {
            throw std::system_error{makeErrorCode(Error::BufferTooSmall), "queue file too small to be a valid queue"};
        }

        auto mapFileResult = MappedRegion::makeMappedRegion(file, pageSize);
        if (!mapFileResult) {
            throw std::system_error{mapFileResult.error(), "failed to map queue file into memory"};
        }

        auto memory = std::move(mapFileResult).value();
        auto buffer = memory.content();

        auto header = std::bit_cast<MemoryHeader const*>(buffer.data());
        if (!std::ranges::equal(MPSCMessageQueueLayout<Options>::kTag, header->tag)) {
            throw std::system_error{makeErrorCode(Error::TagMismatch), "unexpected queue tag value"};
        }

        file_ = std::move(file);
    }

    template <typename... Args>
    [[nodiscard]] static auto makeQueue(
        Args&&... args) noexcept -> std::expected<MPSCMessageQueueImpl<Options>, std::error_code> {
        try {
            return {MPSCMessageQueueImpl{std::forward<Args>(args)...}};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeProducer(Args&&... args) noexcept -> std::expected<Producer, std::error_code> {
        try {
            return {MPSCMessageQueueImpl{std::forward<Args>(args)...}.createProducer()};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeConsumer(Args&&... args) noexcept -> std::expected<Consumer, std::error_code> {
        try {
            return {MPSCMessageQueueImpl{std::forward<Args>(args)...}.createConsumer()};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    /// Return true on queue is initialized
    [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
        return static_cast<bool>(file_);
    }

    /// Create producer for the queue, throws std::system_error on error
    [[nodiscard]] TURBOQ_FORCE_INLINE auto createProducer() -> Producer {
        assert(file_);
        return Producer{MappedRegion{file_}};
    }

    /// Create consumer for the queue, throws std::system_error on error
    [[nodiscard]] TURBOQ_FORCE_INLINE auto createConsumer() -> Consumer {
        assert(file_);
        if (!file_.tryLock()) {
            throw std::system_error{makeErrorCode(Error::ConsumerAlreadyExists), "consumer already exists"};
        }
        return Consumer{MappedRegion{file_}};
    }
};

} // namespace detail

struct MPSCMessageQueueOptionsDefault {
    static constexpr std::string_view tag{"turboq/mpsc"};
};
using MPSCMessageQueue = detail::MPSCMessageQueueImpl<MPSCMessageQueueOptionsDefault>;

} // namespace turboq
