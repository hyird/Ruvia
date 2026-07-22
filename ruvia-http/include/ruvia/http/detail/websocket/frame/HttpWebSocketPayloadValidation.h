#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/websocket/frame/HttpWebSocketFrameCodec.h"

// Whether peer-supplied payload bytes are legal: the Close code registry (RFC
// 6455 section 7.4), UTF-8 well-formedness for Text messages (section 8.1), and
// the two combined for a Close frame's payload.

namespace ruvia::detail {

[[nodiscard]] bool isValidWebSocketCloseCode(std::uint16_t code) noexcept;
[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept;

// The protocol failure a Close payload commits, or nullopt when it is legal.
[[nodiscard]] std::optional<WebSocketProtocolFailure>
webSocketClosePayloadFailure(std::string_view payload) noexcept;
}  // namespace ruvia::detail
