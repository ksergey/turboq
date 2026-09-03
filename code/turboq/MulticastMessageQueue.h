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
struct MulticastMessageQueueLayout {
    static constexpr std::string_view kTag = Options::tag;

    struct MemoryHeader {
        char tag[kTag.size()];
        alignas(kCacheLineSize) std::size_t producerPos;
    };

    struct MessageHeader {
        std::size_t size; // aligned payload size
        std::size_t payloadOffset;
        std::size_t payloadSize;
        // Monotonically increasing per-producer message counter, stamped in prepare(). A consumer
        // that reads a sequence it didn't expect knows this slot has been overwritten one or more
        // times since it last looked -- i.e. it has been lapped by the producer. This is the *only*
        // signal used for overrun detection: there is no separate flow-control counter, by design
        // (this queue intentionally has no backpressure -- a slow consumer must never be able to
        // stall the producer or other consumers).
        //
        // KNOWN LIMITATION: this only checks the sequence once, at fetch() time -- it is not a
        // full seqlock (which would also re-check the sequence *after* the caller finishes reading
        // the payload span fetch() returned, and retry if it changed). Between fetch() returning
        // and the caller finishing that read, the producer could in principle wrap the entire ring
        // and overwrite this exact slot again, and the caller would read a torn mix of the old and
        // new message with no signal that it happened. Closing this fully means either copying the
        // payload out inside the library and re-checking sequence before returning (gives up
        // zero-copy), or pushing a second, post-read check onto the caller (see overrunCount() on
        // the consumer). Not implemented for now: it requires the producer to lap the *entire*
        // ring while a single fetch()'s payload is still being read by the caller, which in
        // practice means the consumer is already badly behind -- at that point the fix is on the
        // consumer side (keep up, or size the ring for your worst-case consumer latency), not a
        // patch here. Revisit if that assumption stops holding.
        std::size_t sequence;
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
    /// Contains tag and producerPos fields
    static constexpr auto kMemoryHeaderBufferSize = makeCacheLineAligned(sizeof(MemoryHeader));

    /// Size of the message header buffer, aligned to cache line
    /// Contains size, payloadOffset, payloadSize, sequence fields
    static constexpr auto kMessageHeaderBufferSize = makeCacheLineAligned(sizeof(MessageHeader));
};

/// Multicast queue producer
template <typename Options>
class MulticastMessageQueueProducerImpl {
private:
    using Layout = MulticastMessageQueueLayout<Options>;
    using MemoryHeader = typename Layout::MemoryHeader;
    using MessageHeader = typename Layout::MessageHeader;

    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::size_t producerPosCache_{0};
    MessageHeader* lastMessageHeader_{nullptr};
    std::size_t nextSequence_{1}; // 1-based; matches the "no prior baseline" sentinel used by the consumer

public:
    MulticastMessageQueueProducerImpl() = default;
    ~MulticastMessageQueueProducerImpl() = default;

    MulticastMessageQueueProducerImpl(MulticastMessageQueueProducerImpl&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          lastMessageHeader_{std::exchange(other.lastMessageHeader_, nullptr)},
          nextSequence_{std::exchange(other.nextSequence_, 1)} {}

    MulticastMessageQueueProducerImpl& operator=(MulticastMessageQueueProducerImpl&& other) noexcept {
        if (this != &other) {
            this->~MulticastMessageQueueProducerImpl();
            new (this) MulticastMessageQueueProducerImpl{std::move(other)};
        }
        return *this;
    }

    MulticastMessageQueueProducerImpl(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        assert(storage_);

        if (auto const rc = storage_.advise(Advice::Sequential); !rc) {
            std::fprintf(stderr, "WARNING: advise failed: %s\n", rc.error().message().c_str());
        }

        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(storage_.data());
        data_ = content.subspan(Layout::kMemoryHeaderBufferSize);
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
        auto const payloadBufferSize = Layout::makeCacheLineAligned(size);
        auto const messageBufferSize = Layout::kMessageHeaderBufferSize + payloadBufferSize;

        assert((messageBufferSize & (kCacheLineSize - 1)) == 0);

        lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + producerPosCache_);
        lastMessageHeader_->size = payloadBufferSize;
        lastMessageHeader_->payloadSize = size;
        lastMessageHeader_->sequence = nextSequence_++;

        // check enough space for current message + additional aligned header
        if (producerPosCache_ + messageBufferSize + Layout::kMessageHeaderBufferSize > data_.size()) [[unlikely]] {
            producerPosCache_ = 0;
        } else {
            producerPosCache_ += Layout::kMessageHeaderBufferSize;
        }

        lastMessageHeader_->payloadOffset = producerPosCache_;
        producerPosCache_ += payloadBufferSize;

        return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
    }

    /// Make reserved buffer visible for consumers
    TURBOQ_FORCE_INLINE void commit() noexcept {
        std::atomic_ref(header_->producerPos).store(producerPosCache_, std::memory_order_release);
    }

    /// \overload
    /// Precondition: \c size <= the \c size passed to the matching prepare(). Passing a \c size larger than
    /// was reserved is UB -- the returned buffer was only sized for the prepare() value, so the
    /// consumer would read past it.
    TURBOQ_FORCE_INLINE void commit(std::size_t size) noexcept {
        // Update payload size
        if (size <= lastMessageHeader_->payloadSize) [[likely]] {
            lastMessageHeader_->payloadSize = size;
        } else {
            assert(false);
        }
        this->commit();
    }
};

/// Multicast queue consumer
template <typename Options>
class MulticastMessageQueueConsumerImpl {
private:
    using Layout = MulticastMessageQueueLayout<Options>;
    using MemoryHeader = typename Layout::MemoryHeader;
    using MessageHeader = typename Layout::MessageHeader;

    MappedRegion storage_;
    std::span<std::byte> data_;
    MemoryHeader* header_{nullptr};
    std::size_t consumerPosCache_{0};
    std::size_t producerPosCache_{0};
    MessageHeader* lastMessageHeader_{nullptr};
    std::size_t expectedSequence_{0};  // meaningful only when haveExpectedSequence_ is true
    bool haveExpectedSequence_{false}; // false right after construction/reset/an overrun: next
                                       // fetch() just baselines on whatever sequence it sees,
                                       // rather than comparing against a stale expectation
    std::size_t overrunCount_{0};      // cumulative count of detected laps, since this consumer was created

public:
    MulticastMessageQueueConsumerImpl() = default;
    ~MulticastMessageQueueConsumerImpl() = default;

    MulticastMessageQueueConsumerImpl(MulticastMessageQueueConsumerImpl&& other) noexcept
        : storage_{std::move(other.storage_)}, data_{std::move(other.data_)},
          header_{std::exchange(other.header_, nullptr)}, consumerPosCache_{std::exchange(other.consumerPosCache_, 0)},
          producerPosCache_{std::exchange(other.producerPosCache_, 0)},
          lastMessageHeader_{std::exchange(other.lastMessageHeader_, nullptr)},
          expectedSequence_{std::exchange(other.expectedSequence_, 0)},
          haveExpectedSequence_{std::exchange(other.haveExpectedSequence_, false)},
          overrunCount_{std::exchange(other.overrunCount_, 0)} {}

    MulticastMessageQueueConsumerImpl& operator=(MulticastMessageQueueConsumerImpl&& other) noexcept {
        if (this != &other) {
            this->~MulticastMessageQueueConsumerImpl();
            new (this) MulticastMessageQueueConsumerImpl{std::move(other)};
        }
        return *this;
    }

    MulticastMessageQueueConsumerImpl(MappedRegion&& storage) noexcept : storage_{std::move(storage)} {
        assert(storage_);

        if (auto const rc = storage_.advise(Advice::Sequential); !rc) {
            std::fprintf(stderr, "WARNING: advise failed: %s\n", rc.error().message().c_str());
        }

        auto content = storage_.content();
        header_ = std::bit_cast<MemoryHeader*>(content.data());
        data_ = content.subspan(Layout::kMemoryHeaderBufferSize);
        consumerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_relaxed);
        producerPosCache_ = consumerPosCache_;

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

    /// Get next buffer for reading. Return empty buffer in case of no data -- including right
    /// after an overrun was detected (see overrunCount()): the position has already been resynced
    /// to the producer's current head, so the next call resumes normally from there.
    [[nodiscard]] TURBOQ_FORCE_INLINE auto fetch() noexcept -> std::span<std::byte const> {
        if (producerPosCache_ == consumerPosCache_ &&
            (producerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire)) ==
                consumerPosCache_) {
            return {};
        }

        lastMessageHeader_ = std::bit_cast<MessageHeader*>(data_.data() + consumerPosCache_);
        auto const sequence = lastMessageHeader_->sequence;

        if (!haveExpectedSequence_) [[unlikely]] {
            haveExpectedSequence_ = true;
        } else if (sequence != expectedSequence_) [[unlikely]] {
            // This slot has been overwritten one or more times since we last looked at it: we've
            // been lapped by the producer. We deliberately don't use payloadOffset/payloadSize from
            // this header at all -- they may belong to a later message we're not interested in, or
            // (if the producer happens to be overwriting this exact slot right now) be a torn read
            // of a header that's actively being written. Either way, jump straight to the
            // producer's current position (same recovery as reset()) instead of trying to figure
            // out how many messages were lost or salvage anything from this slot.
            ++overrunCount_;
            consumerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_acquire);
            producerPosCache_ = consumerPosCache_;
            haveExpectedSequence_ = false;
            return {};
        }

        expectedSequence_ = sequence + 1;
        return data_.subspan(lastMessageHeader_->payloadOffset, lastMessageHeader_->payloadSize);
    }

    /// Consume buffer and make buffer space available for producer
    /// pre: fetch() -> non empty buffer
    TURBOQ_FORCE_INLINE void consume() noexcept {
        assert((reinterpret_cast<std::uintptr_t>(lastMessageHeader_) & (kCacheLineSize - 1)) == 0);
        assert((lastMessageHeader_->payloadOffset & (kCacheLineSize - 1)) == 0);
        assert((lastMessageHeader_->size & (kCacheLineSize - 1)) == 0);

        consumerPosCache_ = lastMessageHeader_->payloadOffset + lastMessageHeader_->size;
    }

    /// Cumulative number of times this consumer detected it had been lapped by the producer (see
    /// fetch()), since it was created or last reset(). This is the only way to observe that an
    /// overrun happened -- fetch() itself just reports an empty buffer either way (see fetch()'s
    /// doc comment), the same as "no data yet". Poll this periodically for monitoring/alerting.
    [[nodiscard]] TURBOQ_FORCE_INLINE auto overrunCount() const noexcept -> std::size_t {
        return overrunCount_;
    }

    /// Reset queue
    TURBOQ_FORCE_INLINE void reset() noexcept {
        consumerPosCache_ = std::atomic_ref(header_->producerPos).load(std::memory_order_relaxed);
        producerPosCache_ = consumerPosCache_;
        haveExpectedSequence_ = false;
    }
};

/// Memory layout:
///
///    MemoryHeader        data_ (ring buffer of variable-size messages, wraps just like SPSC's)
///   +----------------+-----------------------------------------------------------------------+
///   | tag |producerPos|  Header | Payload |  Header | Payload | ...        |    free space    |
///   +----------------+-----------------------------------------------------------------------+
///    cache-line aligned
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
/// Same variable-size ring and wrap-via-payloadOffset trick as SPSCQueue (see SPSCQueue.h for the
/// full explanation) -- but with two deliberate differences that follow from this being a
/// broadcast queue, not a point-to-point one:
///
///  - No consumerPos in MemoryHeader. There's no single "the consumer" position to publish: any
///    number of independent consumers can attach and read the same stream, each tracking its own
///    position purely locally, never written back to shared memory.
///  - No backpressure as a direct consequence: the producer never checks how far behind any
///    consumer is, and will happily overwrite data a slow consumer hasn't read yet -- a slow
///    consumer must never be able to stall the producer or other consumers.
///
/// Since the producer can't be held back, MessageHeader carries one extra field a plain SPSC
/// message doesn't need: `sequence`, a monotonically increasing per-producer counter. A consumer
/// that reads a sequence it didn't expect knows the slot it just looked at has since been
/// overwritten -- i.e. it has been lapped -- and counts that via overrunCount() instead of handing
/// back a stale or out-of-context message. See fetch() below.

template <typename Options>
class MulticastMessageQueueImpl {
private:
    using Layout = MulticastMessageQueueLayout<Options>;
    using MemoryHeader = typename Layout::MemoryHeader;
    using MessageHeader = typename Layout::MessageHeader;

    File file_;

    MulticastMessageQueueImpl(File file) noexcept : file_{std::move(file)} {}

public:
    using Producer = MulticastMessageQueueProducerImpl<Options>;
    using Consumer = MulticastMessageQueueConsumerImpl<Options>;

    struct CreationOptions {
        std::size_t capacityHint;
    };

    MulticastMessageQueueImpl(MulticastMessageQueueImpl const&) = delete;
    MulticastMessageQueueImpl& operator=(MulticastMessageQueueImpl const&) = delete;
    MulticastMessageQueueImpl() = default;

    MulticastMessageQueueImpl(MulticastMessageQueueImpl&& other) noexcept : file_{std::move(other.file_)} {}

    MulticastMessageQueueImpl& operator=(MulticastMessageQueueImpl&& other) noexcept {
        if (this != &other) {
            this->~MulticastMessageQueueImpl();
            new (this) MulticastMessageQueueImpl{std::move(other)};
        }
        return *this;
    }

    /// Construct multicast queue (open or create), throws std::runtime_error on error
    MulticastMessageQueueImpl(std::string_view name, CreationOptions const& options,
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
            std::ranges::copy(Layout::kTag, header->tag);
            std::atomic_ref(header->producerPos).store(0, std::memory_order_relaxed);
        }

        auto header = std::bit_cast<MemoryHeader const*>(buffer.data());
        if (!std::ranges::equal(Layout::kTag, header->tag)) {
            throw std::system_error{makeErrorCode(Error::TagMismatch), "unexpected queue tag value"};
        }

        file_ = std::move(file);
    }

    /// Construct multicast queue (open only), throws std::runtime_error on error
    MulticastMessageQueueImpl(std::string_view name, MemorySource const& memorySource = DefaultMemorySource{}) {
        auto openMemorySourceResult = memorySource.open(name, MemorySource::OpenOnly);
        if (!openMemorySourceResult) {
            throw std::system_error{openMemorySourceResult.error(), "failed to open memory source"};
        }

        auto [file, pageSize] = std::move(openMemorySourceResult).value();

        auto getFileSizeResult = file.tryGetFileSize();
        if (!getFileSizeResult) {
            throw std::system_error{getFileSizeResult.error(), "failed to get queue file size"};
        }
        if (getFileSizeResult.value() < Layout::kMemoryHeaderBufferSize) {
            throw std::system_error{makeErrorCode(Error::BufferTooSmall), "queue file too small to be a valid queue"};
        }

        auto mapFileResult = MappedRegion::makeMappedRegion(file, pageSize);
        if (!mapFileResult) {
            throw std::system_error{mapFileResult.error(), "failed to map queue file into memory"};
        }

        auto memory = std::move(mapFileResult).value();
        auto buffer = memory.content();

        auto header = std::bit_cast<MemoryHeader const*>(buffer.data());
        if (!std::ranges::equal(Layout::kTag, header->tag)) {
            throw std::system_error{makeErrorCode(Error::TagMismatch), "unexpected queue tag value"};
        }

        file_ = std::move(file);
    }

    template <typename... Args>
    [[nodiscard]] static auto makeQueue(Args&&... args) noexcept
        -> std::expected<MulticastMessageQueueImpl<Options>, std::error_code> {
        try {
            return {MulticastMessageQueueImpl{std::forward<Args>(args)...}};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeProducer(Args&&... args) noexcept -> std::expected<Producer, std::error_code> {
        try {
            return {MulticastMessageQueueImpl{std::forward<Args>(args)...}.createProducer()};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    template <typename... Args>
    [[nodiscard]] static auto makeConsumer(Args&&... args) noexcept -> std::expected<Consumer, std::error_code> {
        try {
            return {MulticastMessageQueueImpl{std::forward<Args>(args)...}.createConsumer()};
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
        if (!file_.tryLock()) {
            throw std::system_error{makeErrorCode(Error::ProducerAlreadyExists), "producer already exists"};
        }
        return Producer{MappedRegion{file_}};
    }

    /// Create consumer for the queue, throws std::system_error on error
    [[nodiscard]] TURBOQ_FORCE_INLINE auto createConsumer() -> Consumer {
        assert(file_);
        return Consumer{MappedRegion{file_}};
    }
};

} // namespace detail

struct MulticastMessageQueueOptionsDefault {
    static constexpr std::string_view tag{"turboq/multicast"};
};
using MulticastMessageQueue = detail::MulticastMessageQueueImpl<MulticastMessageQueueOptionsDefault>;

} // namespace turboq
