// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <span>
#include <string_view>

#include "MappedRegion.h"
#include "MemorySource.h"
#include "Platform.h"
#include "detail/math.h"

namespace turboq {
namespace detail {

struct MulticastQueueDetail {
    static constexpr std::string_view kTag{"turboq/multicast"};

    struct MemoryHeader {
        char tag[kTag.size()];
        alignas(kCacheLineSize) std::size_t producerPos;
    };

    struct MessageHeader {
        std::size_t size; // aligned payload size
        std::size_t payloadOffset;
        std::size_t payloadSize;
    };
};

} // namespace detail

/// Multicast queue producer
class MulticastQueueProducer : detail::MulticastQueueDetail {
private:
    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::size_t producerPosCache_{0};
    MessageHeader* lastMessageHeader_{nullptr};

public:
    MulticastQueueProducer() = default;
    ~MulticastQueueProducer() = default;

    MulticastQueueProducer(MulticastQueueProducer&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          lastMessageHeader_{std::exchange(other.lastMessageHeader_, nullptr)} {}

    MulticastQueueProducer& operator=(MulticastQueueProducer&& other) noexcept {
        if (this != &other) {
            this->~MulticastQueueProducer();
            new (this) MulticastQueueProducer{std::move(other)};
        }
        return *this;
    }

    MulticastQueueProducer(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(storage_.data());
        data_ = content.subspan(detail::align_up(sizeof(MemoryHeader), kCacheLineSize));
        producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
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
        constexpr auto headerAlignedSize = detail::align_up(sizeof(MessageHeader), kCacheLineSize);
        auto const payloadAlignedSize = detail::align_up(size, kCacheLineSize);
        auto const alignedSize = headerAlignedSize + payloadAlignedSize;

        lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
        lastMessageHeader_->size = payloadAlignedSize;
        lastMessageHeader_->payloadSize = size;

        // check enought space for current message + additional aligned header
        if (producerPosCache_ + alignedSize + headerAlignedSize > data_.size()) [[unlikely]] {
            producerPosCache_ = 0;
        } else {
            producerPosCache_ += headerAlignedSize;
        }

        lastMessageHeader_->payloadOffset = producerPosCache_;
        producerPosCache_ += payloadAlignedSize;

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
            // This is not necessory change
            lastMessageHeader_->size = detail::align_up(size, kCacheLineSize);
        } else {
            assert(false);
        }
        this->commit();
    }
};

/// Multicast queue consumer
class MulticastQueueConsumer : detail::MulticastQueueDetail {
private:
    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::size_t consumerPosCache_{0};
    std::size_t producerPosCache_{0};
    MessageHeader* lastMessageHeader_{nullptr};

public:
    MulticastQueueConsumer() = default;
    ~MulticastQueueConsumer() = default;

    MulticastQueueConsumer(MulticastQueueConsumer&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, consumerPosCache_{std::exchange(other.consumerPosCache_, 0)},
          producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          lastMessageHeader_{std::exchange(other.lastMessageHeader_, nullptr)} {}

    MulticastQueueConsumer& operator=(MulticastQueueConsumer&& other) noexcept {
        if (this != &other) {
            this->~MulticastQueueConsumer();
            new (this) MulticastQueueConsumer(std::move(other));
        }
        return *this;
    }

    MulticastQueueConsumer(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(content.data());
        data_ = content.subspan(detail::align_up(sizeof(MemoryHeader), kCacheLineSize));
        consumerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_relaxed);
        producerPosCache_ = consumerPosCache_;
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
        assert((lastMessageHeader_->payloadOffset & (kCacheLineSize - 1)) == 0);
        assert((lastMessageHeader_->size & (kCacheLineSize - 1)) == 0);
        consumerPosCache_ = lastMessageHeader_->payloadOffset + lastMessageHeader_->size;
    }

    /// Reset queue
    TURBOQ_FORCE_INLINE void reset() noexcept {
        consumerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_relaxed);
        producerPosCache_ = consumerPosCache_;
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

class MulticastQueue : detail::MulticastQueueDetail {
private:
    File file_;

    MulticastQueue(File file) noexcept : file_{std::move(file)} {}

public:
    using Producer = MulticastQueueProducer;
    using Consumer = MulticastQueueConsumer;

    struct CreationOptions {
        std::size_t capacityHint;
    };

    MulticastQueue(MulticastQueue const&) = delete;
    MulticastQueue& operator=(MulticastQueue const&) = delete;
    MulticastQueue() = default;

    MulticastQueue(MulticastQueue&& other) noexcept : file_{std::move(other.file_)} {}

    MulticastQueue& operator=(MulticastQueue&& other) noexcept {
        if (this != &other) {
            this->~MulticastQueue();
            new (this) MulticastQueue{std::move(other)};
        }
        return *this;
    }

    /// Construct multicast queue (open or create), throws std::runtime_error on error
    MulticastQueue(std::string_view name, CreationOptions const& options,
        MemorySource const& memorySource = DefaultMemorySource{});

    /// Construct multicast queue (open only), throws std::runtime_error on error
    MulticastQueue(std::string_view name, MemorySource const& memorySource = DefaultMemorySource{});

    template <typename... Args>
    [[nodiscard]] static auto makeMulticastQueue(Args&&... args) noexcept
        -> std::expected<MulticastQueue, std::error_code> {
        try {
            return {MulticastQueue{std::forward<Args>(args)...}};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeMulticastQueueProducer(Args&&... args) noexcept
        -> std::expected<MulticastQueueProducer, std::error_code> {
        try {
            return {MulticastQueue{std::forward<Args>(args)...}.createProducer()};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeMulticastQueueConsumer(Args&&... args) noexcept
        -> std::expected<MulticastQueueConsumer, std::error_code> {
        try {
            return {MulticastQueue{std::forward<Args>(args)...}.createConsumer()};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    /// Return true on queue intialized
    [[nodiscard]] TURBOQ_FORCE_INLINE explicit operator bool() const noexcept {
        return static_cast<bool>(file_);
    }

    /// Create producer for the queue, throws std::system_error on error
    [[nodiscard]] auto createProducer() -> Producer;

    /// Create consumer for the queue, throws std::system_error on error
    [[nodiscard]] auto createConsumer() -> Consumer;
};

} // namespace turboq
