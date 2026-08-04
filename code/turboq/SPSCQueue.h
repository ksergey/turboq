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

struct SPSCQueueDetail {
    static constexpr std::string_view kTag{"turboq/spsc"};

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
};

} // namespace detail

class SPSCQueueProducer : detail::SPSCQueueDetail {};

class SPSCQueueConsumer : detail::SPSCQueueDetail {};

class SPSCQueue : detail::SPSCQueueDetail {};

} // namespace turboq
