#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/Hex.h"
#include "ruvia/http/detail/PmrResource.h"

namespace ruvia::detail {

enum class UrlDecodeMode : std::uint8_t {
    kPercent,
    kForm
};

[[nodiscard]] inline bool hasUrlEncoding(std::string_view value, UrlDecodeMode mode) noexcept {
    for (const char c : value) {
        if (c == '%' || (mode == UrlDecodeMode::kForm && c == '+')) {
            return true;
        }
    }
    return false;
}

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

[[nodiscard]] inline bool validateUrlEncoding(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            continue;
        }
        if (decodePercentByte(value, i) < 0) {
            return false;
        }
    }
    return true;
}

template <typename StringT>
[[nodiscard]] bool decodeUrlComponent(std::string_view input, StringT& output, UrlDecodeMode mode) {
    output.clear();
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (mode == UrlDecodeMode::kForm && c == '+') {
            output.push_back(' ');
            continue;
        }
        if (c == '%') {
            const int byte = decodePercentByte(input, i);
            if (byte < 0) {
                return false;
            }
            output.push_back(static_cast<char>(byte));
            continue;
        }
        output.push_back(c);
    }
    return true;
}

[[nodiscard]] inline bool urlComponentEquals(
    std::string_view encoded,
    std::string_view decoded,
    UrlDecodeMode mode) noexcept {
    if (encoded.size() < decoded.size()) {
        return false;
    }

    std::size_t out = 0;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        char c = encoded[i];
        if (mode == UrlDecodeMode::kForm && c == '+') {
            c = ' ';
        } else if (c == '%') {
            const int byte = decodePercentByte(encoded, i);
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

template <typename Visitor>
[[nodiscard]] bool dispatchUrlEncodedPairVisitor(
    Visitor& visitor,
    std::string_view name,
    std::string_view value) {
    if constexpr (requires { { visitor(name, value) } -> std::convertible_to<bool>; }) {
        return static_cast<bool>(visitor(name, value));
    } else {
        visitor(name, value);
        return true;
    }
}

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
            const auto value = equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
            if (!dispatchUrlEncodedPairVisitor(visitorRef, name, value)) {
                return true;
            }
        }

        if (pairEnd == std::string_view::npos) {
            return true;
        }
        input.remove_prefix(pairEnd + 1);
    }
    return true;
}

[[nodiscard]] inline std::optional<std::string_view> findUrlEncodedValue(
    std::string_view input,
    std::string_view decodedName,
    UrlDecodeMode mode) {
    std::optional<std::string_view> result;
    (void)visitUrlEncodedPairs(input, [&](std::string_view name, std::string_view value) {
        if (urlComponentEquals(name, decodedName, mode)) {
            result = value;
        }
        return true;
    });
    return result;
}

[[nodiscard]] inline std::optional<std::pmr::string> decodeUrlComponentToString(
    std::string_view input,
    std::pmr::memory_resource* resource,
    UrlDecodeMode mode) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    if (!decodeUrlComponent(input, output, mode)) {
        return std::nullopt;
    }
    return output;
}

}  // namespace ruvia::detail
