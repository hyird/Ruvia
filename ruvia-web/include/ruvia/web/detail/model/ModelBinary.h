#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/Base64.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/json/JsonString.h"

namespace ruvia::detail {

inline void appendModelBinary(std::pmr::string& output, const Bytes& value) {
    const auto encodedSize = ruvia::base64EncodedSize(value.size());
    const auto start = output.size();
    output.resize(start + encodedSize + 2);
    output[start] = '"';
    ruvia::encodeBase64(output.data() + start + 1, value.view());
    output[start + encodedSize + 1] = '"';
}

[[nodiscard]] inline std::optional<Bytes> decodeModelBinary(
    std::string_view encoded, std::pmr::memory_resource* resource) {
    if (encoded.size() % 4 != 0) {
        return std::nullopt;
    }
    std::size_t padding = 0;
    if (!encoded.empty() && encoded.back() == '=') {
        ++padding;
        if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') {
            ++padding;
        }
    }
    const auto decodedSize = encoded.size() / 4 * 3;
    if (padding > 2 || padding > decodedSize) {
        return std::nullopt;
    }

    auto* const outputResource = pmrResourceOrDefault(resource);
    std::pmr::vector<std::uint8_t> decoded(outputResource);
    decoded.reserve(decodedSize - padding);
    auto valueOf = [](char value) constexpr -> int {
        if (value >= 'A' && value <= 'Z') {
            return value - 'A';
        }
        if (value >= 'a' && value <= 'z') {
            return value - 'a' + 26;
        }
        if (value >= '0' && value <= '9') {
            return value - '0' + 52;
        }
        if (value == '+') {
            return 62;
        }
        if (value == '/') {
            return 63;
        }
        return -1;
    };

    for (std::size_t offset = 0; offset < encoded.size(); offset += 4) {
        const bool last = offset + 4 == encoded.size();
        const char a = encoded[offset];
        const char b = encoded[offset + 1];
        const char c = encoded[offset + 2];
        const char d = encoded[offset + 3];
        const int first = valueOf(a);
        const int second = valueOf(b);
        if (first < 0 || second < 0 || (!last && (c == '=' || d == '='))) {
            return std::nullopt;
        }
        if (c == '=') {
            if (!last || d != '=' || (second & 0x0F) != 0) {
                return std::nullopt;
            }
            decoded.push_back(static_cast<std::uint8_t>((first << 2) | (second >> 4)));
            continue;
        }
        const int third = valueOf(c);
        if (third < 0) {
            return std::nullopt;
        }
        if (d == '=') {
            if (!last || (third & 0x03) != 0) {
                return std::nullopt;
            }
            decoded.push_back(static_cast<std::uint8_t>((first << 2) | (second >> 4)));
            decoded.push_back(static_cast<std::uint8_t>((second << 4) | (third >> 2)));
            continue;
        }
        const int fourth = valueOf(d);
        if (fourth < 0) {
            return std::nullopt;
        }
        decoded.push_back(static_cast<std::uint8_t>((first << 2) | (second >> 4)));
        decoded.push_back(static_cast<std::uint8_t>((second << 4) | (third >> 2)));
        decoded.push_back(static_cast<std::uint8_t>((third << 6) | fourth));
    }
    Bytes result({.resource = outputResource});
    result.assignOwned(std::move(decoded));
    return result;
}

[[nodiscard]] inline std::optional<Bytes> parseModelBinaryValue(
    std::string_view& input, std::pmr::memory_resource* resource) {
    auto remaining = input;
    const auto token = parseJsonString(remaining);
    if (!token.has_value()) {
        return std::nullopt;
    }

    auto* const outputResource = pmrResourceOrDefault(resource);
    std::optional<Bytes> result;
    if (token->encoding() == JsonStringEncoding::kLiteral) {
        result = decodeModelBinary(token->raw(), outputResource);
    } else {
        const auto decoded = decodeJsonString(token->raw(), outputResource);
        if (!decoded.has_value()) {
            return std::nullopt;
        }
        result = decodeModelBinary(*decoded, outputResource);
    }
    if (!result.has_value()) {
        return std::nullopt;
    }
    input = remaining;
    return result;
}

}  // namespace ruvia::detail
