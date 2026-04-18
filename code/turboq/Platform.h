// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <cstddef>
#include <new>

#ifndef TURBOQ_FORCE_INLINE
#define TURBOQ_FORCE_INLINE inline __attribute__((always_inline))
#endif

#ifndef TURBOQ_NO_INLINE
#define TURBOQ_NO_INLINE inline __attribute__((noinline))
#endif

#ifndef TURBOQ_COLD
#define TURBOQ_COLD __attribute__((cold))
#endif

namespace turboq {

/// Cache line size
static constexpr std::size_t kCpuCacheLineSize = 64u;

} // namespace turboq
