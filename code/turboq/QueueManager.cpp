// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: MIT

#include "QueueManager.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>

#include <nlohmann/json.hpp>

namespace turboq {
namespace {

using Json = nlohmann::json;
using namespace std::string_view_literals;

/// Builds error messages of the form "<source>: queues.orders.capacity: <what>"
class ConfigParser {
private:
    std::string_view source_;

    [[noreturn]] TURBOQ_COLD void fail(std::string_view where, std::string_view what) const {
        auto message = std::string{source_};
        if (!where.empty()) {
            message.append(": ").append(where);
        }
        message.append(": ").append(what);
        throw std::system_error{makeErrorCode(Error::InvalidConfig), message};
    }

    static auto join(std::string_view parent, std::string_view key) -> std::string {
        return std::string{parent}.append(".").append(key);
    }

    static auto toLower(std::string_view value) -> std::string {
        std::string result{value};
        std::ranges::transform(result, result.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return result;
    }

    void checkKeys(Json const& object, std::string_view where, std::initializer_list<std::string_view> allowed) const {
        for (auto const& [key, value] : object.items()) {
            if (std::ranges::find(allowed, std::string_view{key}) == allowed.end()) {
                fail(join(where, key), "unknown key");
            }
        }
    }

    void checkName(std::string_view name, std::string_view where) const {
        if (name.empty() || name == "."sv || name == ".."sv || name.find_first_of("/\0"sv) != std::string_view::npos) {
            fail(where, "invalid queue name (must be a valid file name)");
        }
    }

    [[nodiscard]] auto parseString(Json const& value, std::string_view where) const -> std::string {
        if (!value.is_string()) {
            fail(where, "expected string");
        }
        return value.get<std::string>();
    }

    [[nodiscard]] auto parseSize(Json const& value, std::string_view where) const -> std::size_t {
        std::size_t result = 0;

        if (value.is_number_unsigned()) {
            result = value.get<std::size_t>();
        } else if (value.is_string()) {
            auto const text = value.get_ref<std::string const&>();
            auto const* const begin = text.data();
            auto const* const end = begin + text.size();

            auto const [ptr, ec] = std::from_chars(begin, end, result);
            if (ec == std::errc::result_out_of_range) {
                fail(where, "size is too large");
            }
            if (ec != std::errc{}) {
                fail(where, "expected size, e.g. 4096, \"64KiB\", \"16M\", \"1GiB\"");
            }

            auto const suffix = toLower(std::string_view{ptr, end});
            std::size_t multiplier = 1;
            if (suffix.empty()) {
                multiplier = 1;
            } else if (suffix == "k"sv || suffix == "kib"sv) {
                multiplier = std::size_t{1} << 10;
            } else if (suffix == "m"sv || suffix == "mib"sv) {
                multiplier = std::size_t{1} << 20;
            } else if (suffix == "g"sv || suffix == "gib"sv) {
                multiplier = std::size_t{1} << 30;
            } else {
                fail(where, "unknown size suffix (expected K, KiB, M, MiB, G or GiB)");
            }

            if (result > std::numeric_limits<std::size_t>::max() / multiplier) {
                fail(where, "size is too large");
            }
            result *= multiplier;
        } else {
            fail(where, "expected positive integer or size string");
        }

        if (result == 0) {
            fail(where, "size must be greater than zero");
        }
        return result;
    }

    [[nodiscard]] auto parseHugePages(Json const& value, std::string_view where) const -> HugePagesOption {
        auto const text = toLower(parseString(value, where));
        if (text == "none"sv) {
            return HugePagesOption::None;
        }
        if (text == "auto"sv) {
            return HugePagesOption::Auto;
        }
        if (text == "2m"sv) {
            return HugePagesOption::HugePages2M;
        }
        if (text == "1g"sv) {
            return HugePagesOption::HugePages1G;
        }
        fail(where, "expected one of \"none\", \"auto\", \"2m\", \"1g\"");
    }

    [[nodiscard]] auto parseType(Json const& value, std::string_view where) const -> QueueType {
        auto const text = toLower(parseString(value, where));
        for (auto const type : {QueueType::SPSC, QueueType::MPSC, QueueType::Multicast}) {
            if (text == queueTypeName(type)) {
                return type;
            }
        }
        fail(where, "expected one of \"spsc\", \"mpsc\", \"multicast\"");
    }

    [[nodiscard]] auto parseSpec(std::string const& name, Json const& entry) const -> QueueSpec {
        auto const where = join("queues", name);

        checkName(name, where);
        if (!entry.is_object()) {
            fail(where, "expected object");
        }
        checkKeys(
            entry, where, {"type"sv, "capacity"sv, "slotSize"sv, "length"sv, "hugepages"sv, "path"sv, "pageSize"sv});

        QueueSpec spec;
        spec.name = name;

        if (!entry.contains("type")) {
            fail(where, "missing required key \"type\"");
        }
        spec.type = parseType(entry["type"], join(where, "type"));

        auto const require = [&](std::string_view key) -> Json const& {
            if (!entry.contains(key)) {
                fail(where, std::string{"missing required key \""}
                                .append(key)
                                .append("\" for ")
                                .append(queueTypeName(spec.type))
                                .append(" queue"));
            }
            return entry[key];
        };
        auto const forbid = [&](std::string_view key) {
            if (entry.contains(key)) {
                fail(join(where, key),
                    std::string{"not applicable to "}.append(queueTypeName(spec.type)).append(" queue"));
            }
        };

        if (spec.type == QueueType::MPSC) {
            spec.slotSize = parseSize(require("slotSize"), join(where, "slotSize"));
            spec.length = parseSize(require("length"), join(where, "length"));
            forbid("capacity");
        } else {
            spec.capacity = parseSize(require("capacity"), join(where, "capacity"));
            forbid("slotSize");
            forbid("length");
        }

        if (entry.contains("path")) {
            if (entry.contains("hugepages")) {
                fail(where, "\"path\" and \"hugepages\" are mutually exclusive");
            }
            auto path = parseString(entry["path"], join(where, "path"));
            if (path.empty()) {
                fail(join(where, "path"), "must not be empty");
            }
            spec.path = std::move(path);

            if (entry.contains("pageSize")) {
                spec.pageSize = parseSize(entry["pageSize"], join(where, "pageSize"));
                if (!std::has_single_bit(spec.pageSize)) {
                    fail(join(where, "pageSize"), "must be a power of two");
                }
            }
        } else {
            if (entry.contains("pageSize")) {
                fail(join(where, "pageSize"), "requires \"path\"");
            }
            if (entry.contains("hugepages")) {
                spec.hugePages = parseHugePages(entry["hugepages"], join(where, "hugepages"));
            }
        }

        return spec;
    }

public:
    explicit ConfigParser(std::string_view source) noexcept : source_{source} {}

    [[nodiscard]] auto parse(std::string_view text) const -> std::map<std::string, QueueSpec, std::less<>> {
        Json root;
        try {
            root = Json::parse(text.begin(), text.end(), nullptr, /* allow_exceptions */ true,
                /* ignore_comments */ true);
        } catch (Json::parse_error const& e) {
            fail({}, e.what());
        }

        if (!root.is_object()) {
            fail({}, "expected JSON object at top level");
        }
        checkKeys(root, "", {"queues"sv});
        if (!root.contains("queues")) {
            fail({}, "missing required key \"queues\"");
        }

        auto const& queues = root["queues"];
        if (!queues.is_object()) {
            fail("queues", "expected object");
        }

        std::map<std::string, QueueSpec, std::less<>> specs;
        for (auto const& [name, entry] : queues.items()) {
            specs.emplace(name, parseSpec(name, entry));
        }
        return specs;
    }
};

} // namespace

auto queueTypeName(QueueType type) noexcept -> std::string_view {
    switch (type) {
    case QueueType::SPSC: return "spsc";
    case QueueType::MPSC: return "mpsc";
    case QueueType::Multicast: return "multicast";
    default: return "?";
    }
}

auto QueueSpec::makeMemorySource() const -> DefaultMemorySource {
    if (path) {
        return DefaultMemorySource{*path, pageSize != 0 ? pageSize : detail::getDefaultPageSize()};
    }
    return DefaultMemorySource{hugePages};
}

auto QueueManager::parse(std::string_view text) -> Specs {
    return ConfigParser{"<string>"}.parse(text);
}

auto QueueManager::readFile(std::filesystem::path const& path) -> Specs {
    std::ifstream stream{path, std::ios::in | std::ios::binary};
    if (!stream) {
        auto const ec = errno != 0 ? makePosixErrorCode(errno) : makePosixErrorCode(ENOENT);
        throw std::system_error{ec, "failed to open queue config \"" + path.string() + "\""};
    }

    auto const text = std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    if (stream.bad()) {
        throw std::system_error{makePosixErrorCode(EIO), "failed to read queue config \"" + path.string() + "\""};
    }

    return ConfigParser{path.string()}.parse(text);
}

} // namespace turboq
