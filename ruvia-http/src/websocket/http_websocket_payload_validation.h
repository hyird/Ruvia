#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "websocket/http_websocket_frame_codec.h"

// Whether peer-supplied payload bytes are legal: the Close code registry (RFC
// 6455 section 7.4) and a Close frame's payload. Text-message UTF-8
// well-formedness (section 8.1) uses util/http_utf8.h.

namespace ruvia::detail {

[[nodiscard]] bool is_valid_websocket_close_code(std::uint16_t code) noexcept;

// The protocol failure a Close payload commits, or nullopt when it is legal.
[[nodiscard]] std::optional<websocket_protocol_failure> websocket_close_payload_failure(
    std::string_view payload_value) noexcept;
}  // namespace ruvia::detail
