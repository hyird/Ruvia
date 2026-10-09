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

#include "ruvia/core/base64.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/json/json_string.h"
#include "ruvia/web/model_types.h"

namespace ruvia::detail {

inline void append_model_binary(std::pmr::string& output, const bytes& value) {
    const auto encoded_size = ruvia::base64_encoded_size(value.size());
    const auto start = output.size();
    output.resize(start + encoded_size + 2);
    output[start] = '"';
    ruvia::encode_base64(output.data() + start + 1, value.view());
    output[start + encoded_size + 1] = '"';
}

[[nodiscard]] inline std::optional<bytes> decode_model_binary(
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
    const auto decoded_size = encoded.size() / 4 * 3;
    if (padding > 2 || padding > decoded_size) {
        return std::nullopt;
    }

    auto* const output_resource = pmr_resource_or_default(resource);
    std::pmr::vector<std::uint8_t> decoded(output_resource);
    decoded.reserve(decoded_size - padding);
    auto value_of = [](char value) constexpr -> int {
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
        const int first = value_of(a);
        const int second = value_of(b);
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
        const int third = value_of(c);
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
        const int fourth = value_of(d);
        if (fourth < 0) {
            return std::nullopt;
        }
        decoded.push_back(static_cast<std::uint8_t>((first << 2) | (second >> 4)));
        decoded.push_back(static_cast<std::uint8_t>((second << 4) | (third >> 2)));
        decoded.push_back(static_cast<std::uint8_t>((third << 6) | fourth));
    }
    bytes result_value({.resource_ = output_resource});
    result_value.assign_owned(std::move(decoded));
    return result_value;
}

}  // namespace ruvia::detail
