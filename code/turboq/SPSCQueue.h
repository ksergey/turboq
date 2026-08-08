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
#include "MappedRegion.h"
#include "MemorySource.h"
#include "Platform.h"
#include "detail/math.h"

namespace turboq {
namespace detail {

template <typename Options>
struct SPSCQueueLayout {
    static constexpr std::string_view kTag = Options::tag;

    struct MemoryHeader {
        char tag[kTag.size()];
        alignas(kCacheLineSize) std::size_t producerPos;
        alignas(kCacheLineSize) std::size_t consumerPos;
    };

    struct MessageHeader {
        std::size_t size; // aligned payload size
        std::size_t payloadOffset;
        std::size_t payloadSize;
    };

    static_assert(std::atomic_ref<std::size_t>::is_always_lock_free);
    static_assert(std::is_trivially_copyable_v<MemoryHeader>);
    static_assert(std::is_trivially_copyable_v<MessageHeader>);

    static constexpr auto getMemoryHeaderBufferSize() noexcept -> std::size_t {
        return detail::align_up(sizeof(MemoryHeader), kCacheLineSize);
    }

    static constexpr auto getMessageHeaderBufferSize() noexcept -> std::size_t {
        return detail::align_up(sizeof(MessageHeader), kCacheLineSize);
    }

    static constexpr auto adjustMessagePayloadBufferSize(std::size_t payloadSize) noexcept -> std::size_t {
        return detail::align_up(payloadSize, kCacheLineSize);
    }

    static constexpr auto adjustMessageBufferSize(std::size_t payloadSize) noexcept -> std::size_t {
        return getMessageHeaderBufferSize() + adjustMessagePayloadBufferSize(payloadSize);
    }
};

/// SPSC queue producer
template <typename Options>
class SPSCQueueProducerImpl {
private:
    using Details = SPSCQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using MessageHeader = typename Details::MessageHeader;

    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::size_t producerPosCache_{0};
    std::size_t minFreeSpace_{0};
    MessageHeader* lastMessageHeader_{nullptr};

public:
    SPSCQueueProducerImpl() = default;
    ~SPSCQueueProducerImpl() = default;

    SPSCQueueProducerImpl(SPSCQueueProducerImpl&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          minFreeSpace_{std::exchange(other.minFreeSpace_, 0)},
          lastMessageHeader_{std::exchange(other.lastMessageHeader_, nullptr)} {}

    SPSCQueueProducerImpl& operator=(SPSCQueueProducerImpl&& other) noexcept {
        if (this != &other) {
            this->~SPSCQueueProducerImpl();
            new (this) SPSCQueueProducerImpl{std::move(other)};
        }
        return *this;
    }

    SPSCQueueProducerImpl(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        assert(storage_);

        if (auto const rc = storage_.advise(Advice::Sequential); !rc) {
            std::fprintf(stderr, "WARNING: advise failed: %s\n", rc.error().message().c_str());
        }

        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(storage_.data());
        data_ = content.subspan(Details::getMemoryHeaderBufferSize());
        producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);

        auto const consumerPos = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);
        if (consumerPos > producerPosCache_) {
            // queue is empty in case of consumerPos == producerPos
            minFreeSpace_ = consumerPos - producerPosCache_ - 1;
        } else {
            // Reserve space at end for last MessageHeader
            minFreeSpace_ = data_.size() - producerPosCache_ - Details::getMessageHeaderBufferSize();
        }
    }

    /// Return true on initialized
    [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
        return static_cast<bool>(storage_);
    }

    /// Return queue capacity (bytes)
    [[nodiscard]] TURBOQ_FORCE_INLINE auto capacity() const noexcept -> std::size_t {
        return storage_.size();
    }

    /// Reserve contiguous space for writing without making it visible to the consumers
    [[nodiscard]] TURBOQ_FORCE_INLINE auto prepare(std::size_t size) noexcept -> std::span<std::byte> {
        // Aligned buffer size for encoding MessageHeader
        constexpr auto headerBufferSize = Details::getMessageHeaderBufferSize();
        // Aligned buffer size for encoding payload (size)
        auto const payloadBufferSize = Details::adjustMessagePayloadBufferSize(size);
        // Aligned buffer size for encoding whole message
        auto const messageBufferSize = Details::adjustMessageBufferSize(size);

        assert(messageBufferSize >= headerBufferSize + payloadBufferSize);

        if (messageBufferSize <= minFreeSpace_) [[likely]] {
            lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
            lastMessageHeader_->size = payloadBufferSize;
            lastMessageHeader_->payloadSize = size;
            lastMessageHeader_->payloadOffset = producerPosCache_ + headerBufferSize;
            producerPosCache_ += messageBufferSize;
            minFreeSpace_ -= messageBufferSize;

            return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
        }

        auto const consumerPosCache = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);
        if (consumerPosCache > producerPosCache_) {
            // queue is empty in case of consumerPos == producerPos
            minFreeSpace_ = consumerPosCache - producerPosCache_ - 1;

            if (messageBufferSize <= minFreeSpace_) [[likely]] {
                lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
                lastMessageHeader_->size = payloadBufferSize;
                lastMessageHeader_->payloadSize = size;
                lastMessageHeader_->payloadOffset = producerPosCache_ + headerBufferSize;
                producerPosCache_ += messageBufferSize;
                minFreeSpace_ -= messageBufferSize;

                return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
            }
        } else {
            assert(headerBufferSize <= (data_.size() - producerPosCache_));

            minFreeSpace_ = data_.size() - producerPosCache_ - headerBufferSize;

            if (messageBufferSize <= minFreeSpace_) [[likely]] {
                lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
                lastMessageHeader_->size = payloadBufferSize;
                lastMessageHeader_->payloadSize = size;
                lastMessageHeader_->payloadOffset = producerPosCache_ + headerBufferSize;
                producerPosCache_ += messageBufferSize;
                minFreeSpace_ -= messageBufferSize;

                return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
            }

            // align payload to cache-line size when payload starts from beginning
            if (payloadBufferSize < consumerPosCache) {
                lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
                lastMessageHeader_->size = payloadBufferSize;
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
        // TODO: new size could be greater previous but less than lastMessageHeader_->size
        if (size <= lastMessageHeader_->payloadSize) [[likely]] {
            lastMessageHeader_->payloadSize = size;
        } else {
            assert(false);
        }
        this->commit();
    }
};

/// SPSC queue consumer
template <typename Options>
class SPSCQueueConsumerImpl {
private:
    using Details = SPSCQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using MessageHeader = typename Details::MessageHeader;

    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::size_t consumerPosCache_{0};
    std::size_t producerPosCache_{0};
    MessageHeader* lastMessageHeader_{nullptr};

public:
    SPSCQueueConsumerImpl() = default;
    ~SPSCQueueConsumerImpl() = default;

    SPSCQueueConsumerImpl(SPSCQueueConsumerImpl&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, consumerPosCache_{std::exchange(other.consumerPosCache_, 0)},
          producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          lastMessageHeader_{std::exchange(other.lastMessageHeader_, nullptr)} {}

    SPSCQueueConsumerImpl& operator=(SPSCQueueConsumerImpl&& other) noexcept {
        if (this != &other) {
            this->~SPSCQueueConsumerImpl();
            new (this) SPSCQueueConsumerImpl(std::move(other));
        }
        return *this;
    }

    SPSCQueueConsumerImpl(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        assert(storage_);

        if (auto const rc = storage_.advise(Advice::Sequential); !rc) {
            std::fprintf(stderr, "WARNING: advise failed: %s\n", rc.error().message().c_str());
        }

        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(content.data());
        data_ = content.subspan(Details::getMemoryHeaderBufferSize());
        consumerPosCache_ = std::atomic_ref(header_->consumerPos).load(std::memory_order_relaxed);
        producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);

        assert((reinterpret_cast<uintptr_t>(data_.data()) & (kCacheLineSize - 1)) == 0);
    }

    /// Return true on initialized
    [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
        return static_cast<bool>(storage_);
    }

    /// Return queue capacity
    [[nodiscard]] TURBOQ_FORCE_INLINE auto capacity() const noexcept -> std::size_t {
        return storage_.size();
    }

    /// Get next buffer for reading. Return empty buffer in case of no data.
    [[nodiscard]] TURBOQ_FORCE_INLINE auto fetch() noexcept -> std::span<std::byte const> {
        if (consumerPosCache_ == producerPosCache_ &&
            (producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire)) ==
                consumerPosCache_) [[unlikely]] {
            return {};
        }
        lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + consumerPosCache_);
        return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
    }

    /// Consume buffer and make buffer space available for producer
    /// pre: fetch() -> non empty buffer
    TURBOQ_FORCE_INLINE void consume() noexcept {
        assert((reinterpret_cast<std::uintptr_t>(lastMessageHeader_) & (kCacheLineSize - 1)) == 0);
        assert((lastMessageHeader_->payloadOffset & (kCacheLineSize - 1)) == 0);
        assert((lastMessageHeader_->size & (kCacheLineSize - 1)) == 0);

        consumerPosCache_ = lastMessageHeader_->payloadOffset + lastMessageHeader_->size;
        std::atomic_ref(header_->consumerPos).store(consumerPosCache_, std::memory_order_release);
    }

    /// Reset queue
    TURBOQ_FORCE_INLINE void reset() noexcept {
        producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
        consumerPosCache_ = producerPosCache_;
        std::atomic_ref(header_->consumerPos).store(consumerPosCache_, std::memory_order_release);
    }
};

/// Queue layout:
/// s               e   s                      e  s                    e
/// +---------------+---+--------+-+------------+--+--------+-+----------+-----+----
/// | MemoryHeader  |xxx| Header |x|Payload     |xx| Header |x| Payload  |xxxxx|uuuu ...
/// +---------------+---+--------+-+------------+--+--------+-+----------+-----+----
/// s   - start
/// e   - end
/// xxx - padding bytes
/// uuu - unused bytes

template <typename Options>
class SPSCQueueImpl {
private:
    using Details = SPSCQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using MessageHeader = typename Details::MessageHeader;

    File file_;

    explicit SPSCQueueImpl(File file) noexcept : file_{std::move(file)} {}

public:
    using Producer = SPSCQueueProducerImpl<Options>;
    using Consumer = SPSCQueueConsumerImpl<Options>;

    struct CreationOptions {
        std::size_t capacityHint;
    };

    SPSCQueueImpl(SPSCQueueImpl const&) = delete;
    SPSCQueueImpl& operator=(SPSCQueueImpl const&) = delete;
    SPSCQueueImpl() = default;

    SPSCQueueImpl(SPSCQueueImpl&& other) noexcept : file_{std::move(other.file_)} {}

    SPSCQueueImpl& operator=(SPSCQueueImpl&& other) noexcept {
        if (this != &other) {
            this->~SPSCQueueImpl();
            new (this) SPSCQueueImpl{std::move(other)};
        }
        return *this;
    }

    /// Construct spsc queue (open or create), throws std::runtime_error on error
    SPSCQueueImpl(std::string_view name, CreationOptions const& options,
        MemorySource const& memorySource = DefaultMemorySource{}) {
        if (options.capacityHint == 0) {
            throw std::system_error{makeErrorCode(Error::InvalidCreationOptions), "invalid capacity hint value"};
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

        // align up capacity hint to page size
        auto const capacity = detail::align_up(options.capacityHint, pageSize);

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
            std::ranges::copy(SPSCQueueLayout<Options>::kTag, header->tag);
            std::atomic_ref(header->producerPos).store(0, std::memory_order_relaxed);
            std::atomic_ref(header->consumerPos).store(0, std::memory_order_relaxed);
        }

        auto header = std::bit_cast<MemoryHeader const*>(buffer.data());
        if (!std::ranges::equal(SPSCQueueLayout<Options>::kTag, header->tag)) {
            throw std::system_error{makeErrorCode(Error::TagMismatch), "unexpected queue tag value"};
        }

        file_ = std::move(file);
    }

    /// Construct spsc queue (open only), throws std::runtime_error on error
    SPSCQueueImpl(std::string_view name, MemorySource const& memorySource = DefaultMemorySource{}) {
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
            // Too small to hold even the memory header: either the queue was never created, or (for
            // memory sources like AnonymousMemorySource, where "open only" cannot truly look up an
            // existing mapping by name) a fresh, empty backing file was handed to us instead. Reject
            // this cleanly -- mapping and dereferencing it would read past the end of the file and
            // raise SIGBUS rather than a catchable error.
            throw std::system_error{makeErrorCode(Error::BufferTooSmall), "queue file too small to be a valid queue"};
        }

        auto mapFileResult = MappedRegion::makeMappedRegion(file, pageSize);
        if (!mapFileResult) {
            throw std::system_error{mapFileResult.error(), "failed to map queue file into memory"};
        }

        auto memory = std::move(mapFileResult).value();
        auto buffer = memory.content();

        auto header = std::bit_cast<MemoryHeader const*>(buffer.data());
        if (!std::ranges::equal(SPSCQueueLayout<Options>::kTag, header->tag)) {
            throw std::system_error{makeErrorCode(Error::TagMismatch), "unexpected queue tag value"};
        }

        file_ = std::move(file);
    }

    template <typename... Args>
    [[nodiscard]] static auto makeQueue(Args&&... args) noexcept
        -> std::expected<SPSCQueueImpl<Options>, std::error_code> {
        try {
            return {SPSCQueueImpl{std::forward<Args>(args)...}};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeProducer(Args&&... args) noexcept -> std::expected<Producer, std::error_code> {
        try {
            return {SPSCQueueImpl{std::forward<Args>(args)...}.createProducer()};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeConsumer(Args&&... args) noexcept -> std::expected<Consumer, std::error_code> {
        try {
            return {SPSCQueueImpl{std::forward<Args>(args)...}.createConsumer()};
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
        return Consumer{MappedRegion{file_}};
    }
};

} // namespace detail

struct SPSCQueueOptionsDefault {
    static constexpr std::string_view tag{"turboq/spsc"};
};

using SPSCQueue = detail::SPSCQueueImpl<SPSCQueueOptionsDefault>;

} // namespace turboq
