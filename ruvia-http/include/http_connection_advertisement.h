#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "byte_view.h"

namespace ruvia {

// Encoders validate limits before allocation and write into the final owned result.
struct http_origin_advertisement final {
    std::vector<std::string> origins_;
    http_origin_advertisement() noexcept = default;
    http_origin_advertisement(const http_origin_advertisement&) = default;
    http_origin_advertisement(http_origin_advertisement&&) noexcept = default;
    http_origin_advertisement& operator=(const http_origin_advertisement&) = default;
    http_origin_advertisement& operator=(http_origin_advertisement&&) noexcept = default;
};
struct http_alternative_service_advertisement final {
    std::uint32_t stream_id_{0};
    std::string origin_;
    std::string field_value_;
    http_alternative_service_advertisement() noexcept = default;
    http_alternative_service_advertisement(const http_alternative_service_advertisement&) = default;
    http_alternative_service_advertisement(http_alternative_service_advertisement&&) noexcept = default;
    http_alternative_service_advertisement& operator=(const http_alternative_service_advertisement&) = default;
    http_alternative_service_advertisement& operator=(http_alternative_service_advertisement&&) noexcept = default;
};
enum class http_connection_advertisement_error : std::uint8_t {
    malformed_payload,
    invalid_origin,
    invalid_field,
    invalid_stream,
    limit,
    output_too_small,
};
// RFC 8336 / RFC 9412 ORIGIN payload decoding & encoding. Invalid serialized
// origins are ignored on decode; truncated length-delimited entries reject the
// payload. Origin authority and connection coalescing require TLS identity.
[[nodiscard]] std::variant<http_origin_advertisement, http_connection_advertisement_error> decode_http_origin_advertisement(byte_view payload_value);
[[nodiscard]] std::variant<std::size_t, http_connection_advertisement_error> http_origin_payload_size(std::span<const std::string_view> origins, std::size_t max_payload_bytes = 16384);
[[nodiscard]] std::variant<std::size_t, http_connection_advertisement_error> encode_http_origin_payload(mutable_byte_view output, std::span<const std::string_view> origins, std::size_t max_payload_bytes = (std::numeric_limits<std::size_t>::max)());
[[nodiscard]] std::variant<std::vector<std::byte>, http_connection_advertisement_error> encode_http2_origin_frame(std::span<const std::string_view> origins, std::uint32_t max_frame_size = 16384);
// RFC 7838 ALTSVC. Stream 0 carries an explicit origin; a request stream
// carries an empty origin and inherits the request origin. The value is an
// Alt-Svc field value; transport authority and cache policy are external.
[[nodiscard]] std::variant<http_alternative_service_advertisement, http_connection_advertisement_error> decode_http2_alternative_service(std::uint32_t stream_id, byte_view payload_value);
[[nodiscard]] std::variant<std::size_t, http_connection_advertisement_error> http2_alternative_service_payload_size(std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size = 16384);
[[nodiscard]] std::variant<std::size_t, http_connection_advertisement_error> encode_http2_alternative_service_payload(mutable_byte_view output, std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size = 16384);
[[nodiscard]] std::variant<std::vector<std::byte>, http_connection_advertisement_error> encode_http2_alternative_service_frame(std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size = 16384);
}  // namespace ruvia
