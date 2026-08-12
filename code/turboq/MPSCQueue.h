// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

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
#include "FetchResult.h"
#include "MappedRegion.h"
#include "MemorySource.h"
#include "Platform.h"
#include "detail/math.h"

namespace turboq {
namespace detail {

template <typename Options>
struct MPSCQueueLayout {
    static constexpr std::string_view kTag = Options::tag;

    struct MemoryHeader {
        char tag[kTag.size()];
        std::size_t slotSize;
        std::size_t length;
        alignas(kCacheLineSize) std::size_t consumerPos;
        alignas(kCacheLineSize) std::size_t producerPos;
    };

    struct MessageHeader {
        std::size_t payloadSize;
    };

    struct StateHeader {
        alignas(kCacheLineSize) bool committed;
    };

    static_assert(std::atomic_ref<std::size_t>::is_always_lock_free);
    static_assert(std::is_trivially_copyable_v<MemoryHeader>);
    static_assert(std::is_trivially_copyable_v<MessageHeader>);
    static_assert(std::is_trivially_copyable_v<StateHeader>);

    static constexpr auto getMemoryHeaderBufferSize() noexcept -> std::size_t {
        return detail::align_up(sizeof(MemoryHeader), kCacheLineSize);
    }

    static constexpr auto getMessageHeaderBufferSize() noexcept -> std::size_t {
        return detail::align_up(sizeof(MessageHeader), kCacheLineSize);
    }

    static constexpr auto adjustMessageBufferSize(std::size_t payloadSize) noexcept -> std::size_t {
        return getMessageHeaderBufferSize() + payloadSize;
    }

    static constexpr auto adjustSlotBufferSize(std::size_t payloadSize) noexcept -> std::size_t {
        return getMessageHeaderBufferSize() + detail::align_up(payloadSize, kCacheLineSize);
    }
};

/// MPSC queue producer
template <typename Options>
class MPSCQueueProducerImpl {
private:
    using Details = MPSCQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using MessageHeader = typename Details::MessageHeader;
    using StateHeader = typename Details::StateHeader;

    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::span<StateHeader> commitStates_;
    std::size_t producerPosCache_{0};
    std::size_t consumerPosCache_{0};

public:
    MPSCQueueProducerImpl() = default;
    ~MPSCQueueProducerImpl() = default;

    MPSCQueueProducerImpl(MPSCQueueProducerImpl&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, commitStates_{std::move(other.commitStates_)},
          producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          consumerPosCache_{std::exchange(other.consumerPosCache_, 0)} {}

    MPSCQueueProducerImpl& operator=(MPSCQueueProducerImpl&& other) noexcept {
        if (this != &other) {
            this->~MPSCQueueProducerImpl();
            new (this) MPSCQueueProducerImpl{std::move(other)};
        }
        return *this;
    }

    MPSCQueueProducerImpl(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        assert(storage_);

        if (auto const rc = storage_.advise(Advice::Sequential); !rc) {
            std::fprintf(stderr, "WARNING: advise failed: %s\n", rc.error().message().c_str());
        }

        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(storage_.data());

        std::size_t offset = Details::getMemoryHeaderBufferSize();
        data_ = content.subspan(offset, header_->slotSize * header_->length);

        offset += header_->slotSize * header_->length;
        commitStates_ = {std::bit_cast<StateHeader*>(content.data() + offset), header_->length};

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
        constexpr auto headerBufferSize = Details::getMessageHeaderBufferSize();
        auto const messageBufferSize = Details::adjustMessageBufferSize(size);

        if (messageBufferSize > header_->slotSize) [[unlikely]] {
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
        }

        producerPosCache_ = currentProducerPos & (header_->length - 1);
        auto content = data_.data() + producerPosCache_ * header_->slotSize;
        std::bit_cast<MessageHeader*>(content)->payloadSize = size;

        return {content + headerBufferSize, size};
    }

    /// Make reserved buffer visible for consumers
    TURBOQ_FORCE_INLINE void commit() noexcept {
        std::atomic_ref(commitStates_[producerPosCache_].committed).store(true, std::memory_order_release);
    }

    /// \overload
    TURBOQ_FORCE_INLINE void commit(std::size_t size) noexcept {
        auto header = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_ * header_->slotSize);
        if (size <= header->payloadSize) [[likely]] {
            header->payloadSize = size;
        } else {
            assert(false);
        }
        this->commit();
    }
};

/// MPSC queue consumer
template <typename Options>
class MPSCQueueConsumerImpl {
private:
    using Details = MPSCQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using MessageHeader = typename Details::MessageHeader;
    using StateHeader = typename Details::StateHeader;

    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::span<StateHeader> commitStates_;
    std::size_t producerPosCache_{0};
    std::size_t consumerPosCache_{0};
    MessageHeader* lastMessageHeader_{nullptr};
    StateHeader* lastCommitState_{nullptr};

public:
    MPSCQueueConsumerImpl() = default;
    ~MPSCQueueConsumerImpl() = default;

    MPSCQueueConsumerImpl(MPSCQueueConsumerImpl&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, commitStates_{std::move(other.commitStates_)},
          producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          consumerPosCache_{std::exchange(other.consumerPosCache_, 0)},
          lastMessageHeader_{std::exchange(other.lastMessageHeader_, nullptr)},
          lastCommitState_{std::exchange(other.lastCommitState_, nullptr)} {}

    MPSCQueueConsumerImpl& operator=(MPSCQueueConsumerImpl&& other) noexcept {
        if (this != &other) {
            this->~MPSCQueueConsumerImpl();
            new (this) MPSCQueueConsumerImpl{std::move(other)};
        }
        return *this;
    }

    MPSCQueueConsumerImpl(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        assert(storage_);

        if (auto const rc = storage_.advise(Advice::Sequential); !rc) {
            std::fprintf(stderr, "WARNING: advise failed: %s\n", rc.error().message().c_str());
        }

        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(storage_.data());

        std::size_t offset = Details::getMemoryHeaderBufferSize();
        data_ = content.subspan(offset, header_->slotSize * header_->length);

        offset += header_->slotSize * header_->length;
        commitStates_ = {std::bit_cast<StateHeader*>(content.data() + offset), header_->length};

        producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
        consumerPosCache_ = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);

        assert((reinterpret_cast<uintptr_t>(data_.data()) & (kCacheLineSize - 1)) == 0);
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

    /// Get next buffer for reading. Returns a result with size==0 when there is no data yet. This
    /// queue applies backpressure to its producers (see prepare()), so a consumer can never be
    /// lapped -- fetch() here never returns an error result. It's still FetchResult (not a bare
    /// span) so the Consumer interface is uniform across all three queue types.
    [[nodiscard]] TURBOQ_FORCE_INLINE auto fetch() noexcept -> FetchResult {
        if (consumerPosCache_ == producerPosCache_ &&
            (producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire)) ==
                consumerPosCache_) [[unlikely]] {
            return {};
        }

        auto const consumerPos = consumerPosCache_ & (header_->length - 1);

        lastCommitState_ = &commitStates_[consumerPos];
        assert((reinterpret_cast<uintptr_t>(lastCommitState_) & (kCacheLineSize - 1)) == 0);

        if (!std::atomic_ref(lastCommitState_->committed).load(std::memory_order_acquire)) [[unlikely]] {
            return {};
        }

        lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + consumerPos * header_->slotSize);
        assert((reinterpret_cast<uintptr_t>(lastMessageHeader_) & (kCacheLineSize - 1)) == 0);

        return std::span<std::byte const>{
            std::bit_cast<std::byte*>(lastMessageHeader_) + Details::getMessageHeaderBufferSize(),
            lastMessageHeader_->payloadSize};
    }

    /// Consume buffer and make buffer space available for producer
    /// pre: fetch() -> non-empty, non-error result
    TURBOQ_FORCE_INLINE void consume() noexcept {
        assert((reinterpret_cast<std::uintptr_t>(lastMessageHeader_) & (kCacheLineSize - 1)) == 0);

        consumerPosCache_++;
        std::atomic_ref(lastCommitState_->committed).store(false, std::memory_order_release);
        std::atomic_ref(header_->consumerPos).store(consumerPosCache_, std::memory_order_release);
    }

    /// Reset queue
    TURBOQ_FORCE_INLINE void reset() noexcept {
        while (consumerPosCache_ != producerPosCache_) {
            // Drop message.
            std::size_t const consumerPos = consumerPosCache_ & (header_->length - 1);
            lastCommitState_ = &commitStates_[consumerPos];
            std::atomic_ref(lastCommitState_->committed).store(false, std::memory_order_release);
            consumerPosCache_++;
        }
        std::atomic_ref(header_->consumerPos).store(consumerPosCache_, std::memory_order_release);
    }
};

template <typename Options>
class MPSCQueueImpl {
private:
    using Details = MPSCQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using MessageHeader = typename Details::MessageHeader;
    using StateHeader = typename Details::StateHeader;

    File file_;

    explicit MPSCQueueImpl(File file) noexcept : file_{std::move(file)} {}

public:
    using Producer = MPSCQueueProducerImpl<Options>;
    using Consumer = MPSCQueueConsumerImpl<Options>;

    struct CreationOptions {
        std::size_t slotSizeHint;
        std::size_t lengthHint;
    };

    MPSCQueueImpl(MPSCQueueImpl const&) = delete;
    MPSCQueueImpl& operator=(MPSCQueueImpl const&) = delete;
    MPSCQueueImpl() = default;

    MPSCQueueImpl(MPSCQueueImpl&& other) noexcept : file_{std::move(other.file_)} {}

    MPSCQueueImpl& operator=(MPSCQueueImpl&& other) noexcept {
        if (this != &other) {
            this->~MPSCQueueImpl();
            new (this) MPSCQueueImpl{std::move(other)};
        }
        return *this;
    }

    /// Construct mpsc queue (open or create), throws std::runtime_error on error
    MPSCQueueImpl(std::string_view name, CreationOptions const& options,
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

        auto const slotSize = Details::adjustSlotBufferSize(options.slotSizeHint);
        auto const length = detail::upper_pow_2(options.lengthHint);
        auto const capacityHint =
            Details::getMemoryHeaderBufferSize() + slotSize * length + sizeof(StateHeader) * length;
        // round-up requested size to page size
        auto const capacity = detail::align_up(capacityHint, pageSize);

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
            std::ranges::copy(MPSCQueueLayout<Options>::kTag, header->tag);
            header->slotSize = slotSize;
            header->length = length;
            std::atomic_ref(header->producerPos).store(0, std::memory_order_relaxed);
            std::atomic_ref(header->consumerPos).store(0, std::memory_order_relaxed);
        }

        auto header = std::bit_cast<MemoryHeader const*>(buffer.data());
        if (!std::ranges::equal(MPSCQueueLayout<Options>::kTag, header->tag)) {
            throw std::system_error{makeErrorCode(Error::TagMismatch), "unexpected queue tag value"};
        }

        file_ = std::move(file);
    }

    /// Construct multicast queue (open only), throws std::runtime_error on error
    MPSCQueueImpl(std::string_view name, MemorySource const& memorySource = DefaultMemorySource{}) {
        auto openMemorySourceResult = memorySource.open(name, MemorySource::OpenOnly);
        if (!openMemorySourceResult) {
            throw std::system_error{openMemorySourceResult.error(), "failed to open memory source"};
        }

        auto [file, pageSize] = std::move(openMemorySourceResult).value();

        auto getFileSizeResult = file.tryGetFileSize();
        if (!getFileSizeResult) {
            throw std::system_error{getFileSizeResult.error(), "failed to get queue file size"};
        }
        if (getFileSizeResult.value() < Details::getMemoryHeaderBufferSize()) {
            throw std::system_error{makeErrorCode(Error::BufferTooSmall), "queue file too small to be a valid queue"};
        }

        auto mapFileResult = MappedRegion::makeMappedRegion(file, pageSize);
        if (!mapFileResult) {
            throw std::system_error{mapFileResult.error(), "failed to map queue file into memory"};
        }

        auto memory = std::move(mapFileResult).value();
        auto buffer = memory.content();

        auto header = std::bit_cast<MemoryHeader const*>(buffer.data());
        if (!std::ranges::equal(MPSCQueueLayout<Options>::kTag, header->tag)) {
            throw std::system_error{makeErrorCode(Error::TagMismatch), "unexpected queue tag value"};
        }

        file_ = std::move(file);
    }

    template <typename... Args>
    [[nodiscard]] static auto makeQueue(Args&&... args) noexcept
        -> std::expected<MPSCQueueImpl<Options>, std::error_code> {
        try {
            return {MPSCQueueImpl{std::forward<Args>(args)...}};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeProducer(Args&&... args) noexcept -> std::expected<Producer, std::error_code> {
        try {
            return {MPSCQueueImpl{std::forward<Args>(args)...}.createProducer()};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeConsumer(Args&&... args) noexcept -> std::expected<Consumer, std::error_code> {
        try {
            return {MPSCQueueImpl{std::forward<Args>(args)...}.createConsumer()};
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

struct MPSCQueueOptionsDefault {
    static constexpr std::string_view tag{"turboq/mpsc"};
};

using MPSCQueue = detail::MPSCQueueImpl<MPSCQueueOptionsDefault>;

} // namespace turboq
