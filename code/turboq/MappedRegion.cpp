// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#include "MappedRegion.h"

#include <cstdio>
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
            std::fprintf(stderr, "closing mapped region, it may be already unmapped\n");
        }
    }
}

auto MappedRegion::advise(std::size_t offset, std::size_t length, Advice advice) const noexcept
    -> std::expected<void, std::error_code> {
    if (size_ == 0) {
        return std::unexpected(makePosixErrorCode(EFAULT));
    }

    int value;
    switch (advice) {
    case Advice::Normal: {
        value = MADV_NORMAL;
    } break;
    case Advice::Random: {
        value = MADV_RANDOM;
    } break;
    case Advice::Sequential: {
        value = MADV_SEQUENTIAL;
    } break;
    default: return std::unexpected(makePosixErrorCode(EINVAL));
    }

    if (auto const rc = ::madvise(data_ + offset, length, value); rc != 0) {
        return std::unexpected(makePosixErrorCode(errno));
    }
    return {};
}

} // namespace turboq
