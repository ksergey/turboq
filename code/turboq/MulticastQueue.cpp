// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include "MulticastQueue.h"

#include <algorithm>
#include <type_traits>

#include "Error.h"

namespace turboq {

static_assert(std::atomic_ref<std::size_t>::is_always_lock_free);
static_assert(std::is_trivially_copyable_v<detail::MulticastQueueDetail::MemoryHeader>);
static_assert(std::is_trivially_copyable_v<detail::MulticastQueueDetail::MessageHeader>);

namespace {

[[nodiscard]] auto check(std::span<std::byte const> buffer) noexcept -> std::expected<void, std::error_code> {
    using MemoryHeader = detail::MulticastQueueDetail::MemoryHeader;

    if (buffer.size() < detail::align_up(sizeof(MemoryHeader), kCacheLineSize)) {
        return std::unexpected(makeErrorCode(Error::BufferTooSmall));
    }
    auto header = std::bit_cast<MemoryHeader const*>(buffer.data());
    if (!std::ranges::equal(detail::MulticastQueueDetail::kTag, header->tag)) {
        return std::unexpected(makeErrorCode(Error::TagMismatch));
    }
    return {};
}

} // namespace

MulticastQueue::MulticastQueue(
    std::string_view name, CreationOptions const& options, MemorySource const& memorySource) {
    auto openMemorySourceResult = memorySource.open(name, MemorySource::OpenOrCreate);
    if (!openMemorySourceResult) {
        throw std::system_error{openMemorySourceResult.error(), "failed to open memory source"};
    }

    auto [file, pageSize] = std::move(openMemorySourceResult).value();

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

    auto mapFileResult = MappedRegion::makeMappedRegion(file);
    if (!mapFileResult) {
        throw std::system_error{mapFileResult.error(), "failed to map queue file into memory"};
    }

    auto memory = std::move(mapFileResult).value();
    if (fileSize == 0) {
        // init queue internals
        auto header = std::bit_cast<MemoryHeader*>(memory.content().data());
        std::atomic_ref(header->producerPos).store(0, std::memory_order_relaxed);
        std::ranges::copy(kTag, header->tag);
    }
    if (auto result = check(memory.content()); !result) {
        throw std::system_error{result.error(), "queue checks failed"};
    }

    file_ = std::move(file);
}

MulticastQueue::MulticastQueue(std::string_view name, MemorySource const& memorySource) {
    auto openMemorySourceResult = memorySource.open(name, MemorySource::OpenOnly);
    if (!openMemorySourceResult) {
        throw std::system_error{openMemorySourceResult.error(), "failed to open memory source"};
    }
    auto [file, pageSize] = std::move(openMemorySourceResult).value();

    auto mapFileResult = MappedRegion::makeMappedRegion(file);
    if (!mapFileResult) {
        throw std::system_error{mapFileResult.error(), "failed to map queue file into memory"};
    }

    auto memory = std::move(mapFileResult).value();
    if (auto result = check(memory.content()); !result) {
        throw std::system_error{result.error(), "queue checks failed"};
    }

    file_ = std::move(file);
}

auto MulticastQueue::createProducer() -> Producer {
    assert(file_);
    if (!file_.tryLock()) {
        throw std::system_error{makeErrorCode(Error::ProducerAlreadyExists), "producer already exists"};
    }
    return Producer{MappedRegion{file_}};
}

auto MulticastQueue::createConsumer() -> Consumer {
    assert(file_);
    return Consumer{MappedRegion{file_}};
}

} // namespace turboq
