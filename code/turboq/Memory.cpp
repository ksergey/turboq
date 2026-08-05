// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include "Memory.h"

#include <sys/mman.h>

#include "Error.h"

namespace turboq {

auto hintLinearScan(void* ptr, std::size_t size) noexcept -> std::expected<void, std::error_code> {
    if (auto const rc = ::madvise(ptr, size, MADV_SEQUENTIAL); rc != 0) {
        return std::unexpected(makePosixErrorCode(errno));
    }
    return {};
}

} // namespace turboq
