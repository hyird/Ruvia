#pragma once

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/detail/websocket/WebSocketServerNegotiation.h"

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

class HttpWebSocketServerHandshake final {
public:
    [[nodiscard]] const WebSocketServerNegotiation&
    negotiation() const & noexcept {
        return negotiation_;
    }
    [[nodiscard]] const WebSocketServerNegotiation&
    negotiation() const && = delete;

    template <typename Visitor>
    void forEachResponsePart(Visitor&& visitor) const {
        visitor(kHttpWebSocketSwitchingProtocolsPrefix);
        visitor(std::string_view(accept_.data(), accept_.size()));
        visitor(kHttpCrlf);
        if (!negotiation_.subprotocol().empty()) {
            visitor(kHttpWebSocketSubprotocolHeaderPrefix);
            visitor(negotiation_.subprotocol());
            visitor(kHttpCrlf);
        }
        if (!negotiation_.extensions().empty()) {
            visitor(kHttpWebSocketExtensionsHeaderPrefix);
            visitor(negotiation_.extensions());
            visitor(kHttpCrlf);
        }
        visitor(kHttpCrlf);
    }

private:
    friend HttpWebSocketServerHandshake makeHttpWebSocketServerHandshake(
        const HttpRequest&,
        std::string_view) noexcept;

    HttpWebSocketServerHandshake(
        WebSocketAcceptKey accept,
        WebSocketServerNegotiation negotiation) noexcept
        : accept_(accept),
          negotiation_(negotiation) {}

    WebSocketAcceptKey accept_;
    WebSocketServerNegotiation negotiation_;
};

[[nodiscard]] inline HttpWebSocketServerHandshake makeHttpWebSocketServerHandshake(
    const HttpRequest& request,
    std::string_view supportedSubprotocols) noexcept {
    WebSocketAcceptKey accept;
    encodeWebSocketAccept(
        accept,
        requestKnownHeader(request, RequestKnownHeader::kSecWebSocketKey));
    return HttpWebSocketServerHandshake(
        accept,
        makeWebSocketServerNegotiation(request, supportedSubprotocols));
}

}  // namespace ruvia::detail
