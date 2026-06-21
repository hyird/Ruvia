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
#include "ruvia/memory/PmrResource.h"

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

[[nodiscard]] inline bool validateUrlEncoding(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            continue;
        }
        if (i + 2 >= value.size() || decodeHexNibble(value[i + 1]) < 0 ||
            decodeHexNibble(value[i + 2]) < 0) {
            return false;
        }
        i += 2;
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
            if (i + 2 >= input.size()) {
                return false;
            }
            const auto high = decodeHexNibble(input[i + 1]);
            const auto low = decodeHexNibble(input[i + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            output.push_back(static_cast<char>((high << 4) | low));
            i += 2;
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
            if (i + 2 >= encoded.size()) {
                return false;
            }
            const auto high = decodeHexNibble(encoded[i + 1]);
            const auto low = decodeHexNibble(encoded[i + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            c = static_cast<char>((high << 4) | low);
            i += 2;
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
        const auto equals = pair.find('=');
        const auto name = equals == std::string_view::npos ? pair : pair.substr(0, equals);
        const auto value = equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);

        if (!dispatchUrlEncodedPairVisitor(visitorRef, name, value)) {
            return true;
        }

        if (pairEnd == std::string_view::npos) {
            return true;
        }
        input.remove_prefix(pairEnd + 1);
    }
    return true;
}

[[nodiscard]] inline std::optional<std::pmr::string> decodeUrlComponentToString(
    std::string_view input,
    std::pmr::memory_resource* resource,
    UrlDecodeMode mode) {
    std::pmr::string output(pmrResourceOrDefault(resource));
    if (!decodeUrlComponent(input, output, mode)) {
        return std::nullopt;
    }
    return output;
}

}  // namespace ruvia::detail
