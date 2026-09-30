#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/http/detail/util/Hex.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia {

enum class UrlDecodeMode : std::uint8_t { kPercent,
    kForm };

struct UrlDecodeOptions final {
    UrlDecodeMode mode{UrlDecodeMode::kPercent};
    std::pmr::memory_resource* resource{nullptr};
};

// Percent mode treats '+' literally; form mode decodes '+' as a space.
[[nodiscard]] inline bool hasUrlEncoding(std::string_view value, UrlDecodeMode mode) noexcept {
    return std::ranges::any_of(value,
        [mode](char c) noexcept { return c == '%' || (mode == UrlDecodeMode::kForm && c == '+'); });
}

namespace detail {

// Decode the percent-escape at position i, where input[i] == '%'. Returns the
// decoded byte (0-255), or -1 if the escape is truncated or contains a non-hex
// digit. On success, advances i past the two hex digits so a `for (...; ++i)`
// loop lands on the next input character. Single owner of %XX decoding for the
// URL-component helpers below.
[[nodiscard]] inline int decodePercentByte(std::string_view input, std::size_t& i) noexcept {
    if (i + 2 >= input.size()) {
        return -1;
    }
    const auto high = decodeHexNibble(input[i + 1]);
    const auto low = decodeHexNibble(input[i + 2]);
    if (high < 0 || low < 0) {
        return -1;
    }
    i += 2;
    return (high << 4) | low;
}

template <typename Visitor>
[[nodiscard]] bool dispatchUrlEncodedPairVisitor(
    Visitor& visitor, std::string_view name, std::string_view value) {
    if constexpr (requires {
                      { visitor(name, value) } -> std::convertible_to<bool>;
                  }) {
        return static_cast<bool>(visitor(name, value));
    } else {
        visitor(name, value);
        return true;
    }
}

}  // namespace detail

[[nodiscard]] inline bool validateUrlEncoding(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            continue;
        }
        if (detail::decodePercentByte(value, i) < 0) {
            return false;
        }
    }
    return true;
}

// Returns the complete decoded component or no value for malformed percent
// encoding. Percent mode treats '+' literally; form mode converts it to a space.
// The result is allocated from options.resource (or the default PMR resource),
// which must outlive the returned string. On failure, no decoded prefix escapes.
[[nodiscard]] inline std::optional<std::pmr::string> decodeUrlComponent(
    std::string_view input, UrlDecodeOptions options = {}) {
    std::pmr::string output(detail::httpPmrResourceOrDefault(options.resource));
    bool valid = true;
    output.resize_and_overwrite(input.size(), [&](char* bytes, std::size_t) noexcept {
        std::size_t written = 0;
        for (std::size_t i = 0; i < input.size(); ++i) {
            char c = input[i];
            if (options.mode == UrlDecodeMode::kForm && c == '+') {
                c = ' ';
            } else if (c == '%') {
                const int byte = detail::decodePercentByte(input, i);
                if (byte < 0) {
                    valid = false;
                    return std::size_t{0};
                }
                c = static_cast<char>(byte);
            }
            bytes[written++] = c;
        }
        return written;
    });
    if (!valid) {
        return std::nullopt;
    }
    return output;
}

// Compare a percent-encoded component to its decoded value without allocating.
// Percent mode treats '+' literally; form mode converts it to a space. Malformed
// percent escapes never compare equal.
[[nodiscard]] inline bool urlComponentEquals(
    std::string_view encoded, std::string_view decoded, UrlDecodeMode mode) noexcept {
    if (encoded.size() < decoded.size()) {
        return false;
    }

    std::size_t out = 0;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        char c = encoded[i];
        if (mode == UrlDecodeMode::kForm && c == '+') {
            c = ' ';
        } else if (c == '%') {
            const int byte = detail::decodePercentByte(encoded, i);
            if (byte < 0) {
                return false;
            }
            c = static_cast<char>(byte);
        }
        if (out >= decoded.size() || decoded[out] != c) {
            return false;
        }
        ++out;
    }
    return out == decoded.size();
}

// Visit raw, borrowed name/value views for each non-empty pair. A bool-returning
// visitor returning false stops iteration and makes this return false; a void
// visitor always traverses all pairs. Pair traversal does not validate percent
// escapes. The input storage must remain alive while each view is used.
template <typename Visitor>
[[nodiscard]] bool visitUrlEncodedPairs(std::string_view input, Visitor&& visitor) {
    auto& visitorRef = visitor;
    while (!input.empty()) {
        const auto pairEnd = input.find('&');
        const auto pair = pairEnd == std::string_view::npos ? input : input.substr(0, pairEnd);

        // Skip empty segments ("&&", leading/trailing "&") rather than emitting a phantom
        // ("", "") pair : a segment with no bytes carries no field.
        if (!pair.empty()) {
            const auto equals = pair.find('=');
            const auto name = equals == std::string_view::npos ? pair : pair.substr(0, equals);
            const auto value =
                equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
            if (!detail::dispatchUrlEncodedPairVisitor(visitorRef, name, value)) {
                return false;
            }
        }

        if (pairEnd == std::string_view::npos) {
            return true;
        }
        input.remove_prefix(pairEnd + 1);
    }
    return true;
}

// Returns the raw (still percent-encoded) borrowed value of the LAST pair whose
// name matches decodedName. Later duplicates override earlier ones. The value
// must be decoded separately before use; input storage must outlive the view.
[[nodiscard]] inline std::optional<std::string_view> findUrlEncodedValue(
    std::string_view input, std::string_view decodedName, UrlDecodeMode mode) {
    std::optional<std::string_view> result;
    (void)visitUrlEncodedPairs(input, [&](std::string_view name, std::string_view value) {
        if (urlComponentEquals(name, decodedName, mode)) {
            result = value;
        }
        return true;
    });
    return result;
}

template <detail::HttpTemporaryOwningCharString Input>
std::optional<std::string_view> findUrlEncodedValue(Input&&, std::string_view, UrlDecodeMode) = delete;

}  // namespace ruvia
