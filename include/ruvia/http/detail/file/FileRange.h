#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ruvia::detail {

struct HttpByteRange final {
    std::uint64_t offset{0};
    std::uint64_t length{0};
};

[[nodiscard]] inline std::optional<std::uint64_t> httpParseUnsigned(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] inline std::optional<HttpByteRange> httpParseByteRange(
    std::string_view header,
    std::uint64_t size) noexcept {
    constexpr std::string_view prefix = "bytes=";
    if (header.size() <= prefix.size() || header.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }

    auto spec = header.substr(prefix.size());
    if (spec.find(',') != std::string_view::npos || size == 0) {
        return std::nullopt;
    }

    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }

    const auto first = spec.substr(0, dash);
    const auto last = spec.substr(dash + 1);
    if (first.empty()) {
        const auto suffix = httpParseUnsigned(last);
        if (!suffix || *suffix == 0) {
            return std::nullopt;
        }
        const auto length = std::min(*suffix, size);
        return HttpByteRange{size - length, length};
    }

    const auto start = httpParseUnsigned(first);
    if (!start || *start >= size) {
        return std::nullopt;
    }

    std::uint64_t end = size - 1;
    if (!last.empty()) {
        const auto parsedEnd = httpParseUnsigned(last);
        if (!parsedEnd || *parsedEnd < *start) {
            return std::nullopt;
        }
        end = std::min(*parsedEnd, size - 1);
    }

    return HttpByteRange{*start, end - *start + 1};
}

[[nodiscard]] inline bool httpByteRangeSetHasMultiple(std::string_view header) noexcept {
    constexpr std::string_view prefix = "bytes=";
    if (header.size() <= prefix.size() || header.substr(0, prefix.size()) != prefix) {
        return false;
    }
    return header.substr(prefix.size()).find(',') != std::string_view::npos;
}

}  // namespace ruvia::detail
