#pragma once

#include <string_view>

#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"

namespace ruvia::detail {

// HTTP-version-independent result of one server-side WebSocket negotiation. HTTP/1
// and RFC 8441 consume this same immutable value for their response head, and the
// subsequent WsConnection consumes its exact deflate alternative. This prevents
// response metadata and frame RSV1 semantics from being configured separately.
class WebSocketServerNegotiation final {
public:
    [[nodiscard]] std::string_view subprotocol() const noexcept {
        return subprotocol_;
    }

    [[nodiscard]] WebSocketDeflateNegotiation deflate() const noexcept {
        return deflate_;
    }

    [[nodiscard]] std::string_view extensions() const noexcept {
        return webSocketDeflateResponseExtensions(deflate_);
    }

private:
    friend WebSocketServerNegotiation makeWebSocketServerNegotiation(
        const HttpRequest&,
        std::string_view) noexcept;

    WebSocketServerNegotiation(
        std::string_view subprotocol,
        WebSocketDeflateNegotiation deflate) noexcept
        : subprotocol_(subprotocol),
          deflate_(deflate) {}

    std::string_view subprotocol_;
    WebSocketDeflateNegotiation deflate_;
};

[[nodiscard]] inline WebSocketServerNegotiation
makeWebSocketServerNegotiation(
    const HttpRequest& request,
    std::string_view supportedSubprotocols) noexcept {
    return WebSocketServerNegotiation(
        chooseWebSocketSubprotocol(request, supportedSubprotocols),
        webSocketNegotiatePermessageDeflate(request));
}

}  // namespace ruvia::detail
