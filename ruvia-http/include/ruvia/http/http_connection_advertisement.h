#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ruvia {
// The resource must outlive the advertisement and all retained frame results.
// Encoders validate limits before allocation and write into the final owned result.
struct http_origin_advertisement final {
    std::pmr::vector<std::pmr::string> origins_;
    explicit http_origin_advertisement(std::pmr::memory_resource* resource)
        : origins_(resource) {}
};
struct http_alternative_service_advertisement final {
    std::uint32_t stream_id_{0};
    std::pmr::string origin_;
    std::pmr::string field_value_;
    explicit http_alternative_service_advertisement(std::pmr::memory_resource* resource)
        : origin_(resource),
          field_value_(resource) {}
};
enum class http_connection_advertisement_error : std::uint8_t {
    malformed_payload,
    invalid_origin,
    invalid_field,
    invalid_stream,
    limit,
};
// RFC 8336 / RFC 9412 share the ORIGIN payload format. Invalid serialized
// origins are ignored; truncated length-delimited entries reject the payload.
// Origin authority and connection coalescing require the runtime's TLS identity.
[[nodiscard]] std::variant<http_origin_advertisement, http_connection_advertisement_error>
decode_http_origin_advertisement(std::span<const char> payload_value,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<std::pmr::vector<char>, http_connection_advertisement_error>
encode_http2_origin_frame(std::span<const std::string_view> origins,
    std::uint32_t max_frame_size = 16384, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<std::pmr::vector<char>, http_connection_advertisement_error>
encode_http3_origin_frame(std::span<const std::string_view> origins,
    std::size_t max_payload_bytes = 64 * 1024, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
// RFC 7838 ALTSVC. Stream 0 carries an explicit origin; a request stream
// carries an empty origin and inherits the request origin. The value is an
// Alt-Svc field value; transport authority and cache policy are external.
[[nodiscard]] std::variant<http_alternative_service_advertisement, http_connection_advertisement_error>
decode_http2_alternative_service(std::uint32_t stream_id, std::span<const char> payload_value,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<std::pmr::vector<char>, http_connection_advertisement_error>
encode_http2_alternative_service_frame(std::uint32_t stream_id, std::string_view origin, std::string_view field_value,
    std::uint32_t max_frame_size = 16384, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
}  // namespace ruvia
