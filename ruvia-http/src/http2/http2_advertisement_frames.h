#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/http_connection_advertisement.h"

namespace ruvia::detail {

// The single ORIGIN (RFC 8336 §2) and ALTSVC (RFC 7838 §4) encoder. The size
// function validates the frame against `max_frame_size` and returns its payload
// size; the write function fills exactly that many bytes in the caller's
// destination, so the public encoders and the connection serialize in place.
[[nodiscard]] std::variant<std::size_t, http_connection_advertisement_error> http2_origin_payload_size(
    std::span<const std::string_view> origins, std::uint32_t max_frame_size);
void http2_write_origin_payload(std::span<char> payload, std::span<const std::string_view> origins) noexcept;

[[nodiscard]] std::variant<std::size_t, http_connection_advertisement_error> http2_alternative_service_payload_size(
    std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size);
void http2_write_alternative_service_payload(
    std::span<char> payload, std::string_view origin, std::string_view field_value) noexcept;

}  // namespace ruvia::detail
