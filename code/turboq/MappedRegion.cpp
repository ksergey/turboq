// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include "MappedRegion.h"

#include <fmt/format.h>
#include <sys/mman.h>

namespace turboq {

MappedRegion::~MappedRegion() noexcept {
  if (size_ > 0) {
    if (::munmap(data_, size_) != 0) {
      fmt::print(stderr, FMT_STRING("closing mapped region, it may be already unmapped\n"));
    }
  }
}

} // namespace turboq
