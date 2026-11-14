#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <variant>

#include "http_authority.h"
#include "http_connection_advertisement.h"
#include "http_header.h"

// ORIGIN (RFC 8336 / RFC 9412) and ALTSVC (RFC 7838) payload codecs shared by
// the HTTP/2 and HTTP/3 engines and the public advertisement encoders.
namespace ruvia::detail {

inline constexpr std::size_t http_origin_length_prefix_bytes = 2;
inline constexpr std::size_t http_max_prefixed_origin_length = 0xffff;

[[nodiscard]] inline bool reserve_http_prefixed_origin(std::size_t& remaining, std::string_view origin) noexcept {
    if (origin.size() > http_max_prefixed_origin_length || remaining < http_origin_length_prefix_bytes || origin.size() > remaining - http_origin_length_prefix_bytes) {
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
[[nodiscard]] inline std::variant<std::size_t, http_connection_advertisement_error> http_origin_payload_size(std::span<const std::string_view> origins, std::size_t payload_limit) {
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

inline void write_http_origin_payload(std::span<char> payload, std::span<const std::string_view> origins) noexcept {
    auto* out = payload.data();
    for (const auto origin : origins) {
        out = write_http_prefixed_origin(out, origin);
    }
}

// RFC 9113 §4.1 / §5.1.1: 24-bit frame payload limit and 31-bit stream ID limit.
inline constexpr std::uint32_t http2_max_frame_payload_size = 16 * 1024 * 1024 - 1;
inline constexpr std::uint32_t http2_max_stream_id = 0x7fffffffU;

// RFC 7838 §4: ALTSVC on stream 0 carries an origin; on a stream it does not.
[[nodiscard]] constexpr bool is_valid_alternative_service_stream(std::uint32_t stream_id, bool has_origin) noexcept {
    return stream_id <= http2_max_stream_id && (stream_id == 0) == has_origin;
}

[[nodiscard]] inline std::variant<std::size_t, http_connection_advertisement_error> http2_alternative_service_payload_size(std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size) {
    if (!is_valid_alternative_service_stream(stream_id, !origin.empty())) {
        return http_connection_advertisement_error::invalid_stream;
    }
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return http_connection_advertisement_error::invalid_origin;
    }
    if (!is_valid_http_header_value(field_value)) {
        return http_connection_advertisement_error::invalid_field;
    }
    std::size_t remaining = std::min(max_frame_size, http2_max_frame_payload_size);
    if (!reserve_http_prefixed_origin(remaining, origin) || field_value.size() > remaining) {
        return http_connection_advertisement_error::limit;
    }
    return std::size_t{2} + origin.size() + field_value.size();
}

inline void http2_write_alternative_service_payload(std::span<char> payload, std::string_view origin, std::string_view field_value) noexcept {
    auto* out = write_http_prefixed_origin(payload.data(), origin);
    if (!field_value.empty()) {
        std::memcpy(out, field_value.data(), field_value.size());
    }
}

}  // namespace ruvia::detail
