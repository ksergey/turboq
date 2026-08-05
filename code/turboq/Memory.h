// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <expected>
#include <system_error>

namespace turboq {

/// Hint kernel that these pages will be scanned linearly
auto hintLinearScan(void* ptr, std::size_t size) noexcept -> std::expected<void, std::error_code>;

} // namespace turboq
