// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <doctest/doctest.h>

#include "MPSCMessageQueue.h"
#include "MulticastMessageQueue.h"
#include "SPSCMessageQueue.h"
#include "TestUtils.h"

namespace turboq::testing {
namespace {

// Default Options (reserveSpace = 0) must keep the exact layout queues had before reserveSpace
// existed, so files created by older versions still open: no reserved region and unchanged header
// sizes.
static_assert(detail::SPSCMessageQueueLayout<SPSCMessageQueueOptionsDefault>::kMemoryHeaderBufferSize == 192);
static_assert(detail::SPSCMessageQueueLayout<SPSCMessageQueueOptionsDefault>::kDataOffset == 192);
static_assert(detail::MPSCMessageQueueLayout<MPSCMessageQueueOptionsDefault>::kMemoryHeaderBufferSize == 192);
static_assert(detail::MPSCMessageQueueLayout<MPSCMessageQueueOptionsDefault>::kDataOffset == 192);
static_assert(detail::MulticastMessageQueueLayout<MulticastMessageQueueOptionsDefault>::kMemoryHeaderBufferSize == 128);
static_assert(detail::MulticastMessageQueueLayout<MulticastMessageQueueOptionsDefault>::kDataOffset == 128);

// ...and existing header fields must stay where older versions put them (reservedSize may only
// take what used to be padding)
using SPSCHeader = detail::SPSCMessageQueueLayout<SPSCMessageQueueOptionsDefault>::MemoryHeader;
static_assert(offsetof(SPSCHeader, producerPos) == 64 && offsetof(SPSCHeader, consumerPos) == 128);
using MPSCHeader = detail::MPSCMessageQueueLayout<MPSCMessageQueueOptionsDefault>::MemoryHeader;
static_assert(offsetof(MPSCHeader, slotSize) == 16 && offsetof(MPSCHeader, length) == 24);
static_assert(offsetof(MPSCHeader, consumerPos) == 64 && offsetof(MPSCHeader, producerPos) == 128);
using MulticastHeader = detail::MulticastMessageQueueLayout<MulticastMessageQueueOptionsDefault>::MemoryHeader;
static_assert(offsetof(MulticastHeader, producerPos) == 64);

struct NoReserve {
    static constexpr std::string_view tag{"turboq/test-reserve"};
    static constexpr std::size_t reserveSpace = 0;
};

struct Reserve100 {
    static constexpr std::string_view tag{"turboq/test-reserve"};
    static constexpr std::size_t reserveSpace = 100;
};

struct Reserve200 {
    static constexpr std::string_view tag{"turboq/test-reserve"};
    static constexpr std::size_t reserveSpace = 200;
};

struct Reserve8K {
    static constexpr std::string_view tag{"turboq/test-reserve"};
    static constexpr std::size_t reserveSpace = 8192;
};

struct SPSC {
    template <typename Options>
    using Queue = detail::SPSCMessageQueueImpl<Options>;

    template <typename Options>
    static auto options(std::size_t capacityHint = 16384) {
        return typename Queue<Options>::CreationOptions{.capacityHint = capacityHint};
    }
};

struct MPSC {
    template <typename Options>
    using Queue = detail::MPSCMessageQueueImpl<Options>;

    template <typename Options>
    static auto options() {
        return typename Queue<Options>::CreationOptions{.slotSizeHint = 64, .lengthHint = 16};
    }
};

struct Multicast {
    template <typename Options>
    using Queue = detail::MulticastMessageQueueImpl<Options>;

    template <typename Options>
    static auto options(std::size_t capacityHint = 16384) {
        return typename Queue<Options>::CreationOptions{.capacityHint = capacityHint};
    }
};

struct Message {
    std::uint64_t seq;
};

/// MemorySourceFixture as a plain object: a temp dir whose memory source lets independent handles
/// open the same queue file
struct TempMemorySource : MemorySourceFixture {
    using MemorySourceFixture::makeTempMemorySource;
};

TEST_SUITE("ReservedSpace") {

    TEST_CASE_TEMPLATE(
        "reserved region is shared, zero-initialized and independent of the data", Kind, SPSC, MPSC, Multicast) {
        using Queue = typename Kind::template Queue<Reserve100>;

        auto result = Queue::makeQueue("reserve", Kind::template options<Reserve100>(), AnonymousMemorySource{});
        REQUIRE(result);
        auto queue = std::move(result).value();
        auto consumer = queue.createConsumer(); // first: a multicast consumer only sees later messages
        auto producer = queue.createProducer();

        auto const producerRegion = producer.reserved();
        auto const consumerRegion = consumer.reserved();
        REQUIRE_EQ(producerRegion.size(), 100);
        REQUIRE_EQ(consumerRegion.size(), 100);
        CHECK_EQ(reinterpret_cast<std::uintptr_t>(producerRegion.data()) % kCacheLineSize, 0);
        CHECK(std::ranges::all_of(consumerRegion, [](std::byte b) {
            return b == std::byte{0};
        }));

        // written through the producer, visible through the consumer
        std::ranges::fill(producerRegion, std::byte{0xAB});
        CHECK(std::ranges::all_of(consumerRegion, [](std::byte b) {
            return b == std::byte{0xAB};
        }));

        // messages flow as usual (enough of them to wrap the ring several times) and never touch it
        Message msg;
        for (std::uint64_t seq = 0; seq < 1000; ++seq) {
            REQUIRE(enqueue(producer, Message{.seq = seq}));
            REQUIRE(dequeue(consumer, msg));
            REQUIRE_EQ(msg.seq, seq);
        }
        CHECK(std::ranges::all_of(consumerRegion, [](std::byte b) {
            return b == std::byte{0xAB};
        }));

        CHECK_EQ(std::as_const(producer).reserved().data(), producerRegion.data()); // const overload
    }

    TEST_CASE_TEMPLATE("reserveSpace = 0 reserves nothing", Kind, SPSC, MPSC, Multicast) {
        using Queue = typename Kind::template Queue<NoReserve>;

        auto result = Queue::makeQueue("no-reserve", Kind::template options<NoReserve>(), AnonymousMemorySource{});
        REQUIRE(result);
        auto queue = std::move(result).value();
        CHECK(queue.createProducer().reserved().empty());
        CHECK(queue.createConsumer().reserved().empty());
    }

    TEST_CASE_TEMPLATE_DEFINE("opening with a different reserveSpace fails", Kind, reserve_mismatch) {
        TempMemorySource temp;
        auto const memorySource = temp.makeTempMemorySource();

        using Queue100 = typename Kind::template Queue<Reserve100>;
        auto const created = Queue100::makeQueue("mismatch", Kind::template options<Reserve100>(), memorySource);
        REQUIRE(created);

        // open-only: only the recorded reserved size differs
        auto const open200 = Kind::template Queue<Reserve200>::makeQueue("mismatch", memorySource);
        REQUIRE_FALSE(open200);
        CHECK_EQ(open200.error(), makeErrorCode(Error::SizeMismatch));

        auto const openNone = Kind::template Queue<NoReserve>::makeQueue("mismatch", memorySource);
        REQUIRE_FALSE(openNone);
        CHECK_EQ(openNone.error(), makeErrorCode(Error::SizeMismatch));

        // open-or-create with the same creation options
        auto const create200 =
            Kind::template Queue<Reserve200>::makeQueue("mismatch", Kind::template options<Reserve200>(), memorySource);
        REQUIRE_FALSE(create200);
        CHECK_EQ(create200.error(), makeErrorCode(Error::SizeMismatch));

        // and the matching Options still open it
        CHECK(Queue100::makeQueue("mismatch", memorySource));
    }
    TEST_CASE_TEMPLATE_INVOKE(reserve_mismatch, SPSC, MPSC, Multicast);

    TEST_CASE_TEMPLATE("capacity must leave room for the header and the reserved region", Kind, SPSC, Multicast) {
        using Queue = typename Kind::template Queue<Reserve8K>;

        auto const tooSmall =
            Queue::makeQueue("small", Kind::template options<Reserve8K>(4096), AnonymousMemorySource{});
        REQUIRE_FALSE(tooSmall);
        CHECK_EQ(tooSmall.error(), makeErrorCode(Error::InvalidCreationOptions));

        CHECK(Queue::makeQueue("big", Kind::template options<Reserve8K>(16384), AnonymousMemorySource{}));
    }
}

} // namespace
} // namespace turboq::testing
