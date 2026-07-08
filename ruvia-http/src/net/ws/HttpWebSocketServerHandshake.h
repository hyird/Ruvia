#pragma once

#include "HttpRequestFlags.h"
#include "HttpRequestInternal.h"
#include "HttpWebSocketPermessageDeflate.h"
#include "HttpWebSocketUtils.h"

#include <string_view>

namespace ruvia::detail {

inline constexpr std::string_view kHttpWebSocketSwitchingProtocolsPrefix =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: ";
inline constexpr std::string_view kHttpWebSocketSubprotocolHeaderPrefix =
    "Sec-WebSocket-Protocol: ";
inline constexpr std::string_view kHttpWebSocketExtensionsHeaderPrefix =
    "Sec-WebSocket-Extensions: ";
inline constexpr std::string_view kHttpCrlf = "\r\n";

struct HttpWebSocketServerHandshake final {
    WebSocketAcceptKey accept{};
    std::string_view subprotocol;
    std::string_view extensions;
    bool permessageDeflate{false};
};

[[nodiscard]] inline HttpWebSocketServerHandshake makeHttpWebSocketServerHandshake(
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view supportedSubprotocols) noexcept {
    HttpWebSocketServerHandshake handshake;
    encodeWebSocketAccept(
        handshake.accept,
        requestKnownHeader(request, RequestKnownHeader::kSecWebSocketKey));
    handshake.subprotocol = chooseWebSocketSubprotocol(request, flags, supportedSubprotocols);
    const auto deflate = webSocketNegotiatePermessageDeflate(request);
    handshake.permessageDeflate = deflate.enabled;
    if (deflate.enabled) {
        handshake.extensions = deflate.echoServerMaxWindowBits
            ? kWebSocketDeflateResponseExtensionsMaxWindow
            : kWebSocketDeflateResponseExtensions;
    }
    return handshake;
}

}  // namespace ruvia::detail
