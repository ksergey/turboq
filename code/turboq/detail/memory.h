// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include "../File.h"
#include "../MappedRegion.h"

namespace turboq::detail {

/// Map file to memory
[[nodiscard]] auto mapFile(File const& file, std::size_t fileSize) -> MappedRegion;

/// \overload
[[nodiscard]] auto mapFile(File const& file) -> MappedRegion;

} // namespace turboq::detail
