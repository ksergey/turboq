// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <turboq/File.h>
#include <turboq/MappedRegion.h>

namespace turboq::detail {

MappedRegion mapFile(File const& file, std::size_t fileSize);
MappedRegion mapFile(File const& file);

} // namespace turboq::detail
