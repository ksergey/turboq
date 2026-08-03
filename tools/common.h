// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <ctime>

#include <turboq/Platform.h>

namespace utils {

TURBOQ_FORCE_INLINE auto clockNow() noexcept -> ::timespec {
    ::timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

} // namespace utils
