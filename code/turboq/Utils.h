// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include "Platform.h"

namespace turboq {

/// Hint to the CPU that we're in a spin-wait loop (e.g. waiting on fetch()/prepare() to stop
/// returning empty). On x86 this is the PAUSE instruction: it doesn't make the calling thread do
/// less work per se, but it tells the core two useful things: (1) don't bother speculating past
/// this point, since we're about to re-check the same memory location we just read -- avoids
/// wasted speculative work and, more importantly, the memory-order mis-speculation penalty that
/// tight polling loops are prone to; (2) yield front-end/execution resources to the other logical
/// core on the same physical core (hyperthreading), which matters a lot if the producer or
/// another consumer happens to be its sibling. Without it, a busy-spin loop actively starves its
/// hyperthread sibling for no benefit -- PAUSE is close to free for the spinning thread itself
/// (a few cycles of extra latency per iteration on modern CPUs) and meaningfully better for
/// everything sharing the core with it.
///
/// This is a hint, not a synchronization primitive: it provides no memory ordering guarantees on
/// its own. Always pair it with an actual acquire load of whatever you're waiting on (fetch()'s
/// FetchResult, or a raw atomic) -- e.g.:
///
///   for (;;) {
///       auto result = consumer.fetch();
///       if (!result.empty()) {
///           break;
///       }
///       turboq::cpuRelax();
///   }
TURBOQ_FORCE_INLINE void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    // No known equivalent for this architecture: fall back to a compiler barrier so the loop
    // condition is still guaranteed to be re-read from memory rather than hoisted/optimized away.
    asm volatile("" ::: "memory");
#endif
}

} // namespace turboq
