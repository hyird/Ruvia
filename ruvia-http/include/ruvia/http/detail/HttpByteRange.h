#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace ruvia::detail {

struct HttpByteRange final {
    std::uint64_t offset{0};
    std::uint64_t length{0};
};

enum class HttpRangeOutcome : std::uint8_t {
    kSatisfiable,
    kUnsatisfiable,
    kIgnore,
};

struct HttpByteRangeResult final {
    HttpRangeOutcome outcome{HttpRangeOutcome::kIgnore};
    HttpByteRange range{};
};

[[nodiscard]] inline std::optional<std::uint64_t> httpParseByteRangeUnsigned(
    std::string_view value) noexcept {
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

[[nodiscard]] inline HttpByteRangeResult httpParseByteRange(
    std::string_view header,
    std::uint64_t size) noexcept {
    constexpr std::string_view prefix = "bytes=";
    if (header.size() <= prefix.size() || header.substr(0, prefix.size()) != prefix) {
        return {HttpRangeOutcome::kIgnore, {}};
    }

    const auto spec = header.substr(prefix.size());
    if (spec.find(',') != std::string_view::npos) {
        return {HttpRangeOutcome::kIgnore, {}};
    }

    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return {HttpRangeOutcome::kIgnore, {}};
    }

    const auto first = spec.substr(0, dash);
    const auto last = spec.substr(dash + 1);
    if (first.empty()) {
        const auto suffix = httpParseByteRangeUnsigned(last);
        if (!suffix) {
            return {HttpRangeOutcome::kIgnore, {}};
        }
        if (*suffix == 0 || size == 0) {
            return {HttpRangeOutcome::kUnsatisfiable, {}};
        }
        const auto length = std::min(*suffix, size);
        return {HttpRangeOutcome::kSatisfiable, HttpByteRange{size - length, length}};
    }

    const auto start = httpParseByteRangeUnsigned(first);
    if (!start) {
        return {HttpRangeOutcome::kIgnore, {}};
    }
    std::uint64_t end = 0;
    if (!last.empty()) {
        const auto parsedEnd = httpParseByteRangeUnsigned(last);
        if (!parsedEnd || *parsedEnd < *start) {
            return {HttpRangeOutcome::kIgnore, {}};
        }
        end = *parsedEnd;
    }

    if (*start >= size) {
        return {HttpRangeOutcome::kUnsatisfiable, {}};
    }
    const auto clampedEnd = last.empty() ? size - 1 : std::min(end, size - 1);
    return {HttpRangeOutcome::kSatisfiable, HttpByteRange{*start, clampedEnd - *start + 1}};
}

[[nodiscard]] inline bool httpByteRangeSetHasMultiple(std::string_view header) noexcept {
    constexpr std::string_view prefix = "bytes=";
    if (header.size() <= prefix.size() || header.substr(0, prefix.size()) != prefix) {
        return false;
    }
    return header.substr(prefix.size()).find(',') != std::string_view::npos;
}

}  // namespace ruvia::detail
