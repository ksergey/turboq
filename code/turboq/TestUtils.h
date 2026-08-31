// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <type_traits>

#include "Concepts.h"
#include "MemorySource.h"

namespace turboq::testing {

template <typename ProducerT, typename DataT>
    requires Producer<ProducerT> and std::is_trivially_copyable_v<DataT>
[[nodiscard]] auto enqueue(ProducerT& producer, DataT const& data) -> bool {
    auto buffer = producer.prepare(sizeof(data));
    if (buffer.empty()) {
        return false;
    }
    *std::bit_cast<DataT*>(buffer.data()) = data;
    producer.commit();
    return true;
}

template <typename ConsumerT, typename DataT>
    requires Consumer<ConsumerT> and std::is_trivially_copyable_v<DataT>
[[nodiscard]] auto dequeue(ConsumerT& consumer, DataT& data) -> bool {
    auto buffer = consumer.fetch();
    if (buffer.empty()) {
        return false;
    }
    data = *std::bit_cast<DataT const*>(buffer.data());
    consumer.consume();
    return true;
}

template <typename ConsumerT, typename DataT>
    requires Consumer<ConsumerT> and std::is_trivially_copyable_v<DataT>
[[nodiscard]] auto fetch(ConsumerT& consumer, DataT& data) -> bool {
    auto buffer = consumer.fetch();
    if (buffer.empty()) {
        return false;
    }
    data = *std::bit_cast<DataT const*>(buffer.data());
    return true;
}

class MemorySourceFixture {
private:
    std::filesystem::path tempPath_;

public:
    MemorySourceFixture() {
        auto pattern = (std::filesystem::temp_directory_path() / "turboq_test_XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::system_error{errno, std::generic_category(), "mkdtemp(...)"};
        }
        tempPath_ = pattern;
    }

    ~MemorySourceFixture() {
        if (std::filesystem::exists(tempPath_)) {
            std::error_code ec;
            std::filesystem::remove_all(tempPath_, ec);
            if (!ec) {
                std::fprintf(
                    stderr, "failed to remove temp directory (%s): %s\n", tempPath_.c_str(), ec.message().c_str());
            }
        }
    }

protected:
    /// Creates a fresh, uniquely-named temporary directory and returns a
    /// DefaultMemorySource backed by it. Unlike AnonymousMemorySource (where every
    /// open() call creates a brand new memfd, regardless of the requested name),
    /// this lets independent open() calls for the same queue name actually share
    /// the same backing file -- required for tests that open the same queue
    /// through two independent handles (tag/size mismatch, single-consumer /
    /// single-producer enforcement, open-only on an existing queue, etc).
    [[nodiscard]] inline auto makeTempMemorySource() -> DefaultMemorySource {
        return DefaultMemorySource{tempPath_};
    }
};

} // namespace turboq::testing
