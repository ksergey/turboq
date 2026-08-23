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

namespace turboq {
namespace detail {

template <typename Options>
struct SPSCMessageQueueLayout {
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

    /// Aligns size to cache line boundary
    /// Used to prevent false sharing between producer and consumer
    static constexpr auto makeCacheLineAligned(std::size_t size) noexcept -> std::size_t {
        return alignUp(size, kCacheLineSize);
    }

    /// Size of the memory header buffer, aligned to cache line
    /// Contains tag, producerPos, consumerPos fields
    static constexpr auto kMemoryHeaderBufferSize = makeCacheLineAligned(sizeof(MemoryHeader));

    /// Size of the message header buffer, aligned to cache line
    /// Contains size, payloadOffset, payloadSize fields
    static constexpr auto kMessageHeaderBufferSize = makeCacheLineAligned(sizeof(MessageHeader));
};

/// SPSC queue producer
template <typename Options>
class SPSCMessageQueueProducerImpl {
private:
    using Details = SPSCMessageQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using MessageHeader = typename Details::MessageHeader;

    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::size_t producerPosCache_{0};
    std::size_t minFreeSpace_{0};
    MessageHeader* lastMessageHeader_{nullptr};

public:
    SPSCMessageQueueProducerImpl() = default;
    ~SPSCMessageQueueProducerImpl() = default;

    SPSCMessageQueueProducerImpl(SPSCMessageQueueProducerImpl&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          minFreeSpace_{std::exchange(other.minFreeSpace_, 0)},
          lastMessageHeader_{std::exchange(other.lastMessageHeader_, nullptr)} {}

    SPSCMessageQueueProducerImpl& operator=(SPSCMessageQueueProducerImpl&& other) noexcept {
        if (this != &other) {
            this->~SPSCMessageQueueProducerImpl();
            new (this) SPSCMessageQueueProducerImpl{std::move(other)};
        }
        return *this;
    }

    SPSCMessageQueueProducerImpl(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        assert(storage_);

        if (auto const rc = storage_.advise(Advice::Sequential); !rc) {
            std::fprintf(stderr, "WARNING: advise failed: %s\n", rc.error().message().c_str());
        }

        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(storage_.data());
        data_ = content.subspan(Details::kMemoryHeaderBufferSize);
        producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);

        auto const consumerPos = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);
        if (consumerPos > producerPosCache_) {
            // queue is empty in case of consumerPos == producerPos
            minFreeSpace_ = consumerPos - producerPosCache_ - 1;
        } else {
            // Reserve space at end for last MessageHeader
            minFreeSpace_ = data_.size() - producerPosCache_ - Details::kMessageHeaderBufferSize;
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
        auto const payloadBufferSize = Details::makeCacheLineAligned(size);
        auto const messageBufferSize = Details::kMessageHeaderBufferSize + payloadBufferSize;

        assert((messageBufferSize & (kCacheLineSize - 1)) == 0);

        if (messageBufferSize <= minFreeSpace_) [[likely]] {
            lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
            lastMessageHeader_->size = payloadBufferSize;
            lastMessageHeader_->payloadSize = size;
            lastMessageHeader_->payloadOffset = producerPosCache_ + Details::kMessageHeaderBufferSize;
            producerPosCache_ += messageBufferSize;
            minFreeSpace_ -= messageBufferSize;

            return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
        }

        auto const consumerPosCache = std::atomic_ref(header_->consumerPos).load(std::memory_order_acquire);

        if (consumerPosCache > producerPosCache_) {
            // consumer has passed producer: queue is empty or wrapping
            minFreeSpace_ = consumerPosCache - producerPosCache_ - 1;
        } else {
            // producer has not lapsed consumer: space at end of buffer
            minFreeSpace_ = data_.size() - producerPosCache_ - Details::kMessageHeaderBufferSize;
        }

        if (messageBufferSize <= minFreeSpace_) [[likely]] {
            lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
            lastMessageHeader_->size = payloadBufferSize;
            lastMessageHeader_->payloadSize = size;
            lastMessageHeader_->payloadOffset = producerPosCache_ + Details::kMessageHeaderBufferSize;
            producerPosCache_ += messageBufferSize;
            minFreeSpace_ -= messageBufferSize;

            return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
        }

        // align payload to cache-line size when payload starts from beginning (wrap-around case)
        if (payloadBufferSize < consumerPosCache) {
            lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
            lastMessageHeader_->size = payloadBufferSize;
            lastMessageHeader_->payloadSize = size;
            lastMessageHeader_->payloadOffset = 0;
            producerPosCache_ = lastMessageHeader_->size;
            minFreeSpace_ = consumerPosCache - producerPosCache_ - 1;

            return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
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
class SPSCMessageQueueConsumerImpl {
private:
    using Details = SPSCMessageQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using MessageHeader = typename Details::MessageHeader;

    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::size_t consumerPosCache_{0};
    std::size_t producerPosCache_{0};
    MessageHeader* lastMessageHeader_{nullptr};

public:
    SPSCMessageQueueConsumerImpl() = default;
    ~SPSCMessageQueueConsumerImpl() = default;

    SPSCMessageQueueConsumerImpl(SPSCMessageQueueConsumerImpl&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, consumerPosCache_{std::exchange(other.consumerPosCache_, 0)},
          producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          lastMessageHeader_{std::exchange(other.lastMessageHeader_, nullptr)} {}

    SPSCMessageQueueConsumerImpl& operator=(SPSCMessageQueueConsumerImpl&& other) noexcept {
        if (this != &other) {
            this->~SPSCMessageQueueConsumerImpl();
            new (this) SPSCMessageQueueConsumerImpl(std::move(other));
        }
        return *this;
    }

    SPSCMessageQueueConsumerImpl(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        assert(storage_);

        if (auto const rc = storage_.advise(Advice::Sequential); !rc) {
            std::fprintf(stderr, "WARNING: advise failed: %s\n", rc.error().message().c_str());
        }

        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(content.data());
        data_ = content.subspan(Details::kMemoryHeaderBufferSize);
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

/// Memory layout:
///
///    MemoryHeader                                    data_ (ring buffer of variable-size messages)
///   +--------------------------------+---------------------------------------------------------------+
///   | tag | producerPos | consumerPos| Header | Payload | Header | Payload | ...  |    free space    |
///   +--------------------------------+---------------------------------------------------------------+
///    each field cache-line aligned    ^ each Header/Payload pair cache-line aligned
///
/// producerPos/consumerPos are byte offsets into data_ (not message counts). Every message is a
/// {MessageHeader, payload} pair; MessageHeader::payloadOffset points at where its payload
/// actually lives, so a header need not be immediately followed by its own payload -- that's what
/// makes wrap-around work without a separate "wrapped" flag:
///
/// s               e   s                      e  s                    e
/// +---------------+---+--------+-+------------+--+--------+-+----------+-----+----
/// | MemoryHeader  |xxx| Header |x|Payload     |xx| Header |x| Payload  |xxxxx|uuuu ...
/// +---------------+---+--------+-+------------+--+--------+-+----------+-----+----
/// s   - start
/// e   - end
/// xxx - padding bytes (alignment)
/// uuu - unused tail bytes
///
/// When a message doesn't fit in the remaining space before the end of data_, the producer still
/// writes its Header at the current (tail) position -- there must always be room for at least a
/// bare header there, see minFreeSpace_ -- but sets that header's payloadOffset to 0 and places
/// the payload at the very start of data_ instead of right after the header. The consumer follows
/// payloadOffset wherever it points, so this single embedded pointer *is* the wrap marker; the
/// bytes left over at the tail (uuu above) are simply never revisited until the ring wraps again.

template <typename Options>
class SPSCMessageQueueImpl {
private:
    using Details = SPSCMessageQueueLayout<Options>;
    using MemoryHeader = typename Details::MemoryHeader;
    using MessageHeader = typename Details::MessageHeader;

    // Distinct bytes of the same fd, locked independently via File::tryLockRegion() (fcntl
    // F_OFD_SETLK) -- unlike flock() (whole-file only, one lock per fd) this lets producer and
    // consumer each get their own single-holder enforcement on the SAME open file, including when
    // both live in this same process (see File.h's tryLockRegion() doc comment for why plain
    // fcntl F_SETLK record locks can't be used safely for that same-process case).
    static constexpr std::size_t kProducerLockOffset = 0;
    static constexpr std::size_t kConsumerLockOffset = 1;

    File file_;
    // In-process guard against calling createProducer()/createConsumer() twice on the SAME
    // handle: tryLockRegion() re-locking from the same fd/OFD succeeds (it's a re-assertion of a
    // lock this open file description already holds, not a conflict) -- it only protects against
    // a genuinely different fd (a different process, or a separately-opened handle). These flags
    // close that specific gap.
    bool producerCreated_{false};
    bool consumerCreated_{false};

    SPSCMessageQueueImpl(File file) noexcept : file_{std::move(file)} {}

public:
    using Producer = SPSCMessageQueueProducerImpl<Options>;
    using Consumer = SPSCMessageQueueConsumerImpl<Options>;

    struct CreationOptions {
        std::size_t capacityHint;
    };

    SPSCMessageQueueImpl(SPSCMessageQueueImpl const&) = delete;
    SPSCMessageQueueImpl& operator=(SPSCMessageQueueImpl const&) = delete;
    SPSCMessageQueueImpl() = default;

    SPSCMessageQueueImpl(SPSCMessageQueueImpl&& other) noexcept
        : file_{std::move(other.file_)}, producerCreated_{std::exchange(other.producerCreated_, false)},
          consumerCreated_{std::exchange(other.consumerCreated_, false)} {}

    SPSCMessageQueueImpl& operator=(SPSCMessageQueueImpl&& other) noexcept {
        if (this != &other) {
            this->~SPSCMessageQueueImpl();
            new (this) SPSCMessageQueueImpl{std::move(other)};
        }
        return *this;
    }

    /// Construct spsc queue (open or create), throws std::runtime_error on error
    SPSCMessageQueueImpl(std::string_view name, CreationOptions const& options,
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
        auto const capacity = alignUp(options.capacityHint, pageSize);

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
            std::ranges::copy(SPSCMessageQueueLayout<Options>::kTag, header->tag);
            std::atomic_ref(header->producerPos).store(0, std::memory_order_relaxed);
            std::atomic_ref(header->consumerPos).store(0, std::memory_order_relaxed);
        }

        auto header = std::bit_cast<MemoryHeader const*>(buffer.data());
        if (!std::ranges::equal(SPSCMessageQueueLayout<Options>::kTag, header->tag)) {
            throw std::system_error{makeErrorCode(Error::TagMismatch), "unexpected queue tag value"};
        }

        file_ = std::move(file);
    }

    /// Construct spsc queue (open only), throws std::runtime_error on error
    SPSCMessageQueueImpl(std::string_view name, MemorySource const& memorySource = DefaultMemorySource{}) {
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
        if (!std::ranges::equal(SPSCMessageQueueLayout<Options>::kTag, header->tag)) {
            throw std::system_error{makeErrorCode(Error::TagMismatch), "unexpected queue tag value"};
        }

        file_ = std::move(file);
    }

    template <typename... Args>
    [[nodiscard]] static auto makeQueue(Args&&... args) noexcept
        -> std::expected<SPSCMessageQueueImpl<Options>, std::error_code> {
        try {
            return {SPSCMessageQueueImpl{std::forward<Args>(args)...}};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeProducer(Args&&... args) noexcept -> std::expected<Producer, std::error_code> {
        try {
            return {SPSCMessageQueueImpl{std::forward<Args>(args)...}.createProducer()};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeConsumer(Args&&... args) noexcept -> std::expected<Consumer, std::error_code> {
        try {
            return {SPSCMessageQueueImpl{std::forward<Args>(args)...}.createConsumer()};
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
        if (producerCreated_ || !file_.tryLockRegion(kProducerLockOffset)) {
            throw std::system_error{makeErrorCode(Error::ProducerAlreadyExists), "producer already exists"};
        }
        producerCreated_ = true;
        return Producer{MappedRegion{file_}};
    }

    /// Create consumer for the queue, throws std::system_error on error
    [[nodiscard]] TURBOQ_FORCE_INLINE auto createConsumer() -> Consumer {
        assert(file_);
        if (consumerCreated_ || !file_.tryLockRegion(kConsumerLockOffset)) {
            throw std::system_error{makeErrorCode(Error::ConsumerAlreadyExists), "consumer already exists"};
        }
        consumerCreated_ = true;
        return Consumer{MappedRegion{file_}};
    }
};

} // namespace detail

struct SPSCMessageQueueOptionsDefault {
    static constexpr std::string_view tag{"turboq/spsc"};
};
using SPSCMessageQueue = detail::SPSCMessageQueueImpl<SPSCMessageQueueOptionsDefault>;

} // namespace turboq
