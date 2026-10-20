// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "Error.h"
#include "MPSCMessageQueue.h"
#include "MemorySource.h"
#include "MulticastMessageQueue.h"
#include "SPSCMessageQueue.h"

namespace turboq {

/// Queue kind as written in the config ("spsc", "mpsc", "multicast")
enum class QueueType { SPSC, MPSC, Multicast };

/// Return config name of the queue type
[[nodiscard]] auto queueTypeName(QueueType type) noexcept -> std::string_view;

namespace detail {

template <typename T>
struct QueueTypeOf;

template <typename Options>
struct QueueTypeOf<SPSCMessageQueueImpl<Options>> {
    static constexpr QueueType value = QueueType::SPSC;
};

template <typename Options>
struct QueueTypeOf<MPSCMessageQueueImpl<Options>> {
    static constexpr QueueType value = QueueType::MPSC;
};

template <typename Options>
struct QueueTypeOf<MulticastMessageQueueImpl<Options>> {
    static constexpr QueueType value = QueueType::Multicast;
};

} // namespace detail

/// Checks T is one of turboq queue types (any Options)
template <typename T>
concept ConfigurableQueue = requires { detail::QueueTypeOf<T>::value; };

/// Description of one queue from the config
struct QueueSpec {
    /// Queue name, also the backing file name inside the memory source directory
    std::string name;

    /// Queue kind
    QueueType type{QueueType::SPSC};

    /// SPSC / Multicast: ring size in bytes (CreationOptions::capacityHint)
    std::size_t capacity{0};

    /// MPSC: max message size (CreationOptions::slotSizeHint)
    std::size_t slotSize{0};

    /// MPSC: number of slots (CreationOptions::lengthHint)
    std::size_t length{0};

    /// Where the backing file lives when path is not set
    HugePagesOption hugePages{HugePagesOption::None};

    /// Explicit directory for the backing file, overrides hugePages
    std::optional<std::filesystem::path> path;

    /// Page size used with path; 0 means the system default page size
    std::size_t pageSize{0};

    /// Construct memory source described by the spec, throws std::system_error on error
    [[nodiscard]] auto makeMemorySource() const -> DefaultMemorySource;
};

/// Set of queues read from a JSON config file. get() returns a queue by name using the parameters
/// from the config, so producers and consumers in different processes agree on them. Whether get()
/// may create a missing queue is chosen once, when the manager is constructed:
///
///   QueueManager{path}                 -- open only (same as kOpenOnly)
///   QueueManager{path, kOpenOnly}      -- get() opens existing queues only
///   QueueManager{path, kOpenOrCreate}  -- get() opens a queue, or creates it if missing
///
/// Config format (comments are allowed, unknown keys are rejected):
///
///   {
///     "queues": {
///       "orders":  { "type": "spsc", "capacity": "64MiB", "hugepages": "auto" },
///       "events":  { "type": "mpsc", "slotSize": 256, "length": 65536 },
///       "md":      { "type": "multicast", "capacity": "1GiB", "path": "/dev/hugepages1G", "pageSize": "1GiB" }
///     }
///   }
///
/// Sizes are a positive integer or a string with an optional binary suffix: K/KiB, M/MiB, G/GiB.
/// "hugepages" is one of "none" (default), "auto", "2m", "1g". "path" (+ optional "pageSize") selects
/// the directory explicitly and can't be combined with "hugepages".
class QueueManager {
private:
    using Specs = std::map<std::string, QueueSpec, std::less<>>;

    Specs specs_;
    MemorySource::OpenFlags openFlags_{MemorySource::OpenOnly};

    QueueManager(Specs specs, MemorySource::OpenFlags openFlags) noexcept
        : specs_{std::move(specs)}, openFlags_{openFlags} {}

    /// Read and parse config file, throws std::system_error on error
    [[nodiscard]] static auto readFile(std::filesystem::path const& path) -> Specs;

    /// Parse config text, throws std::system_error (Error::InvalidConfig) on error
    [[nodiscard]] static auto parse(std::string_view text) -> Specs;

    template <typename Queue>
    [[nodiscard]] auto getSpecFor(std::string_view name) const -> QueueSpec const& {
        auto const* spec = find(name);
        if (!spec) {
            throw std::system_error{makeErrorCode(Error::QueueNotFound), std::string{name}};
        }
        if (spec->type != detail::QueueTypeOf<Queue>::value) {
            throw std::system_error{makeErrorCode(Error::QueueTypeMismatch),
                std::string{name} + ": configured as " + std::string{queueTypeName(spec->type)} + ", requested " +
                    std::string{queueTypeName(detail::QueueTypeOf<Queue>::value)}};
        }
        return *spec;
    }

public:
    /// Empty manager (no queues), open only
    QueueManager() = default;

    /// Read config file, get() opens existing queues only. Throws std::system_error on error.
    explicit QueueManager(std::filesystem::path const& path) : QueueManager{path, kOpenOnly} {}

    /// Read config file, get() opens existing queues only. Throws std::system_error on error.
    QueueManager(std::filesystem::path const& path, OpenOnly) : QueueManager{readFile(path), MemorySource::OpenOnly} {}

    /// Read config file, get() opens a queue or creates it if missing. Throws std::system_error on error.
    QueueManager(std::filesystem::path const& path, OpenOrCreate)
        : QueueManager{readFile(path), MemorySource::OpenOrCreate} {}

    /// Exception safe constructors
    template <typename... Args>
    [[nodiscard]] static auto makeQueueManager(Args&&... args) noexcept
        -> std::expected<QueueManager, std::error_code> {
        try {
            return {QueueManager{std::forward<Args>(args)...}};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }

    /// Parse config from JSON text, open only. Throws std::system_error (Error::InvalidConfig) on error.
    [[nodiscard]] static auto fromString(std::string_view text) -> QueueManager {
        return fromString(text, kOpenOnly);
    }

    /// \overload
    [[nodiscard]] static auto fromString(std::string_view text, OpenOnly) -> QueueManager {
        return QueueManager{parse(text), MemorySource::OpenOnly};
    }

    /// \overload
    [[nodiscard]] static auto fromString(std::string_view text, OpenOrCreate) -> QueueManager {
        return QueueManager{parse(text), MemorySource::OpenOrCreate};
    }

    /// Return MemorySource::OpenOrCreate if get() may create missing queues, MemorySource::OpenOnly otherwise
    [[nodiscard]] auto openFlags() const noexcept -> MemorySource::OpenFlags {
        return openFlags_;
    }

    /// Return spec for the queue or nullptr if not in config
    [[nodiscard]] auto find(std::string_view name) const noexcept -> QueueSpec const* {
        auto const found = specs_.find(name);
        return found != specs_.end() ? &found->second : nullptr;
    }

    /// Return true if the queue is in config
    [[nodiscard]] auto contains(std::string_view name) const noexcept -> bool {
        return specs_.contains(name);
    }

    /// All queues from the config, ordered by name
    [[nodiscard]] auto specs() const noexcept -> Specs const& {
        return specs_;
    }

    /// Return the queue by name, throws std::system_error on error.
    /// Opens an existing queue only, or opens it and creates if missing, depending on how the manager was
    /// constructed (see openFlags()). Fails with Error::QueueNotFound if the name isn't in config and
    /// Error::QueueTypeMismatch if it is configured as a different queue kind.
    template <ConfigurableQueue Queue>
    [[nodiscard]] auto get(std::string_view name) const -> Queue {
        auto const& spec = getSpecFor<Queue>(name);
        auto const memorySource = spec.makeMemorySource();

        if (openFlags_ == MemorySource::OpenOnly) {
            return Queue{spec.name, memorySource};
        }

        if constexpr (detail::QueueTypeOf<Queue>::value == QueueType::MPSC) {
            return Queue{spec.name,
                typename Queue::CreationOptions{.slotSizeHint = spec.slotSize, .lengthHint = spec.length},
                memorySource};
        } else {
            return Queue{spec.name, typename Queue::CreationOptions{.capacityHint = spec.capacity}, memorySource};
        }
    }

    /// Exception safe version of get()
    template <ConfigurableQueue Queue>
    [[nodiscard]] auto tryGet(std::string_view name) const noexcept -> std::expected<Queue, std::error_code> {
        try {
            return {get<Queue>(name)};
        } catch (std::system_error const& e) {
            return std::unexpected(e.code());
        }
    }
};

} // namespace turboq
