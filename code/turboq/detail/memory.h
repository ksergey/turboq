// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include "../File.h"
#include "../MappedRegion.h"

namespace turboq::detail {

/// Map file to memory
MappedRegion mapFile(File const& file, std::size_t fileSize);

/// \overload
MappedRegion mapFile(File const& file);

} // namespace turboq::detail
