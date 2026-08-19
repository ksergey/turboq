// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#pragma once

namespace turboq {

/// Flag indicating that payload is not aligned to cache line boundary
inline constexpr int kFlagUnalignedPayload = (1 << 0);

} // namespace turboq
