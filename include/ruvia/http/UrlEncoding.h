#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia::detail {

enum class UrlDecodeMode : std::uint8_t {
    kPercent,
    kForm
};

[[nodiscard]] inline int urlHexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

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
        if (i + 2 >= value.size() || urlHexValue(value[i + 1]) < 0 || urlHexValue(value[i + 2]) < 0) {
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
            const auto high = urlHexValue(input[i + 1]);
            const auto low = urlHexValue(input[i + 2]);
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
    if (!hasUrlEncoding(encoded, mode)) {
        return encoded == decoded;
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
            const auto high = urlHexValue(encoded[i + 1]);
            const auto low = urlHexValue(encoded[i + 2]);
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

[[nodiscard]] inline std::optional<std::pmr::string> decodeUrlComponentToString(
    std::string_view input,
    std::pmr::memory_resource* resource,
    UrlDecodeMode mode) {
    std::pmr::string output(resource == nullptr ? std::pmr::get_default_resource() : resource);
    if (!decodeUrlComponent(input, output, mode)) {
        return std::nullopt;
    }
    return output;
}

}  // namespace ruvia::detail
