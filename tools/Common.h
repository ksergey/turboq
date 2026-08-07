// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0
//
// Shared between rate_producer / rate_consumer: wire format, monotonic clock
// helper and a SIGINT/SIGTERM latch so both tools can be interrupted cleanly.

#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>

#include <turboq/Platform.h>

namespace bench {

/// Wire format written into each queue slot. If the requested message size is
/// larger than sizeof(Message), the remaining bytes are left as-is (padding).
struct Message {
    std::uint64_t seq;       // 1-based, strictly monotonically increasing
    std::int64_t sendTimeNs; // steady_clock timestamp, captured right before commit()
};

/// Monotonic clock. Producer and consumer must run on the same host for the
/// resulting latency values to be meaningful (no clock sync across hosts).
[[nodiscard]] inline auto clockNow() noexcept -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Simple SIGINT/SIGTERM latch so both tools can be interrupted cleanly
/// instead of being killed mid-write/mid-read.
class SignalHandler {
private:
    alignas(turboq::kCacheLineSize) std::atomic<bool> terminationRequested_{false};

    SignalHandler() = default;

public:
    SignalHandler(SignalHandler const&) = delete;
    SignalHandler& operator=(SignalHandler const&) = delete;

    [[nodiscard]] static auto instance() noexcept -> SignalHandler& {
        static SignalHandler handler;
        return handler;
    }

    [[nodiscard]] auto terminationRequested() const noexcept -> bool {
        return terminationRequested_.load(std::memory_order_relaxed);
    }

    void installSignalHandlers() noexcept {
        std::signal(SIGINT, &handleSignal);
        std::signal(SIGTERM, &handleSignal);
    }

private:
    static void handleSignal(int) noexcept {
        instance().terminationRequested_.store(true, std::memory_order_relaxed);
    }
};

[[nodiscard]] inline auto signalHandler() noexcept -> SignalHandler& {
    return SignalHandler::instance();
}

} // namespace bench
