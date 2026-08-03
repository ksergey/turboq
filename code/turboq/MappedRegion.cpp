// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include "MappedRegion.h"

#include <print>

#include <sys/mman.h>

#include "Error.h"

namespace turboq {

MappedRegion::MappedRegion(File const& file) : MappedRegion{file, file.getFileSize()} {}

MappedRegion::MappedRegion(File const& file, std::size_t fileSize) {
    auto region = ::mmap(nullptr, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, file.get(), 0);
    if (region == MAP_FAILED) {
        throw std::system_error{makePosixErrorCode(errno), "mmap failed"};
    }
    data_ = static_cast<std::byte*>(region);
    size_ = fileSize;
}

MappedRegion::~MappedRegion() noexcept {
    if (size_ > 0) {
        if (::munmap(data_, size_) != 0) {
            std::print(stderr, "closing mapped region, it may be already unmapped\n");
        }
    }
}

} // namespace turboq
