#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_request_target.h"

namespace ruvia::detail {

inline constexpr std::size_t http_origin_length_prefix_bytes = 2;
inline constexpr std::size_t http_max_prefixed_origin_length = 0xffff;

[[nodiscard]] inline bool reserve_http_prefixed_origin(
    std::size_t& remaining, std::string_view origin) noexcept {
    if (origin.size() > http_max_prefixed_origin_length ||
        remaining < http_origin_length_prefix_bytes ||
        origin.size() > remaining - http_origin_length_prefix_bytes) {
        return false;
    }
    remaining -= http_origin_length_prefix_bytes + origin.size();
    return true;
}

inline char* write_http_prefixed_origin(char* out, std::string_view origin) noexcept {
    *out++ = static_cast<char>(origin.size() >> 8);
    *out++ = static_cast<char>(origin.size());
    if (!origin.empty()) {
        std::memcpy(out, origin.data(), origin.size());
    }
    return out + origin.size();
}

// Shared RFC 8336 §2 / RFC 9412 §2 ORIGIN payload validator and serializer.
[[nodiscard]] inline std::variant<std::size_t, http_connection_advertisement_error>
http_origin_payload_size(std::span<const std::string_view> origins, std::size_t payload_limit) {
    std::size_t remaining = payload_limit;
    for (const auto origin : origins) {
        if (!is_valid_http_serialized_origin(origin)) {
            return http_connection_advertisement_error::invalid_origin;
        }
        if (!reserve_http_prefixed_origin(remaining, origin)) {
            return http_connection_advertisement_error::limit;
        }
    }
    return payload_limit - remaining;
}

inline void write_http_origin_payload(
    std::span<char> payload, std::span<const std::string_view> origins) noexcept {
    auto* out = payload.data();
    for (const auto origin : origins) {
        out = write_http_prefixed_origin(out, origin);
    }
}

}  // namespace ruvia::detail
