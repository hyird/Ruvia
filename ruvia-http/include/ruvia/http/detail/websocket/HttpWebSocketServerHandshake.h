#pragma once

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"

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

    template <typename Visitor>
    void forEachResponsePart(Visitor&& visitor) const {
        visitor(kHttpWebSocketSwitchingProtocolsPrefix);
        visitor(std::string_view(accept.data(), accept.size()));
        visitor(kHttpCrlf);
        if (!subprotocol.empty()) {
            visitor(kHttpWebSocketSubprotocolHeaderPrefix);
            visitor(subprotocol);
            visitor(kHttpCrlf);
        }
        if (!extensions.empty()) {
            visitor(kHttpWebSocketExtensionsHeaderPrefix);
            visitor(extensions);
            visitor(kHttpCrlf);
        }
        visitor(kHttpCrlf);
    }
};

[[nodiscard]] inline HttpWebSocketServerHandshake makeHttpWebSocketServerHandshake(
    const HttpRequest& request,
    std::string_view supportedSubprotocols) noexcept {
    HttpWebSocketServerHandshake handshake;
    encodeWebSocketAccept(
        handshake.accept,
        requestKnownHeader(request, RequestKnownHeader::kSecWebSocketKey));
    handshake.subprotocol = chooseWebSocketSubprotocol(
        request, supportedSubprotocols);
    const auto deflate = webSocketNegotiatePermessageDeflate(request);
    handshake.permessageDeflate = deflate.enabled;
    handshake.extensions = webSocketDeflateResponseExtensions(deflate);
    return handshake;
}

}  // namespace ruvia::detail
