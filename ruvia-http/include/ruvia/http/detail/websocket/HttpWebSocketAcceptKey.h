#pragma once

#include <array>
#include <string_view>

namespace ruvia::detail {

// The base64-encoded SHA-1 of the client key plus the RFC 6455 GUID, as it
// appears in the Sec-WebSocket-Accept response field (always 28 bytes).
using WebSocketAcceptKey = std::array<char, 28>;

void encodeWebSocketAccept(WebSocketAcceptKey& output, std::string_view key);

}  // namespace ruvia::detail
