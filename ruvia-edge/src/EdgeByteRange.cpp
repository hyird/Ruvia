#include "ruvia/edge/detail/EdgeByteRange.h"

#include <charconv>
#include <system_error>

namespace ruvia::edge {

namespace {

[[nodiscard]] std::string_view trimSpace(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] bool parseWholeNumber(std::string_view value, std::size_t& out) {
    if (value.empty()) {
        return false;
    }
    const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

}  // namespace

ByteRange parseSingleByteRange(std::string_view header, std::size_t length) {
    ByteRange range;
    constexpr std::string_view prefix = "bytes=";
    if (!header.starts_with(prefix)) {
        return range;
    }
    std::string_view spec = trimSpace(header.substr(prefix.size()));
    if (spec.find(',') != std::string_view::npos) {
        return range;  // multi-range: not supported, serve full
    }
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return range;
    }
    const std::string_view startText = trimSpace(spec.substr(0, dash));
    const std::string_view endText = trimSpace(spec.substr(dash + 1));

    if (startText.empty()) {
        // Suffix range: the last N bytes.
        std::size_t suffix = 0;
        if (!parseWholeNumber(endText, suffix)) {
            return range;
        }
        if (suffix == 0 || length == 0) {
            range.unsatisfiable = true;
            return range;
        }
        range.start = suffix >= length ? 0 : length - suffix;
        range.end = length - 1;
        range.satisfiable = true;
        return range;
    }

    std::size_t start = 0;
    if (!parseWholeNumber(startText, start)) {
        return range;
    }
    if (start >= length) {
        range.unsatisfiable = true;
        return range;
    }
    std::size_t end = length - 1;
    if (!endText.empty()) {
        if (!parseWholeNumber(endText, end)) {
            return range;
        }
        if (end >= length) {
            end = length - 1;
        }
        if (end < start) {
            return range;  // malformed
        }
    }
    range.start = start;
    range.end = end;
    range.satisfiable = true;
    return range;
}

}  // namespace ruvia::edge
