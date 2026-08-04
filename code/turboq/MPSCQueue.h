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

struct MPSCQueueDetail {
    static constexpr std::string_view kTag{"turboq/mpsc"};

    struct MemoryHeader {
        char tag[kTag.size()];
        std::size_t maxMessageSize;
        std::size_t length;
        alignas(kCacheLineSize) std::size_t consumerPos;
        alignas(kCacheLineSize) std::size_t producerPos;
    };

    struct MessageHeader {
        std::size_t payloadSize;
    };

    struct StateHeader {
        alignas(kCacheLineSize) bool commited;
    };
};

} // namespace detail

class MPSCQueueProducer : detail::MPSCQueueDetail {};

class MPSCQueueConsumer : detail::MPSCQueueDetail {};

class MPSCQueue : detail::MPSCQueueDetail {};

} // namespace turboq
