// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include "memory.h"

#include <sys/mman.h>

#include <system_error>

namespace turboq::detail {

MappedRegion mapFile(File const& file, std::size_t fileSize) {
  auto region = ::mmap(nullptr, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, file.get(), 0);
  if (region == MAP_FAILED) {
    throw std::system_error(errno, getPosixErrorCategory(), "mmap(...)");
  }
  return MappedRegion(static_cast<std::byte*>(region), fileSize);
}

MappedRegion mapFile(File const& file) {
  return mapFile(file, file.getFileSize());
}

} // namespace turboq::detail
