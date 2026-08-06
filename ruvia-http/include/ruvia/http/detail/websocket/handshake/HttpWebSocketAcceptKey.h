#pragma once

#include <array>
#include <string_view>

namespace ruvia::detail {

// The base64-encoded SHA-1 of the client key plus the RFC 6455 GUID, as it
// appears in the Sec-WebSocket-Accept response field (always 28 bytes).
using WebSocketAcceptKey = std::array<char, 28>;

// Throws std::length_error before inspecting the view when key + the RFC
// GUID cannot be represented by the SHA-1 64-bit bit-length field or by the
// size_t arithmetic used by the incremental implementation.
void encodeWebSocketAccept(WebSocketAcceptKey& output, std::string_view key);

}  // namespace ruvia::detail
