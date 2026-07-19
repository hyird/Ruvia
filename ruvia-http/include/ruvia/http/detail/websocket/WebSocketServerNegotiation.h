#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/websocket/HttpWebSocketHandshakeFields.h"
#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"

namespace ruvia::detail {

// HTTP-version-independent result of one server-side WebSocket negotiation. HTTP/1
// and RFC 8441 consume this same immutable value for their response head, and the
// subsequent WsConnection consumes its exact deflate alternative. This prevents
// response metadata and frame RSV1 semantics from being configured separately.
// The selected subprotocol is copied into caller-selected PMR storage so this
// committed value never borrows mutable route configuration.
class WebSocketServerNegotiation final {
public:
    WebSocketServerNegotiation(const WebSocketServerNegotiation&) = delete;
    WebSocketServerNegotiation& operator=(
        const WebSocketServerNegotiation&) = delete;
    WebSocketServerNegotiation(WebSocketServerNegotiation&&) noexcept = default;
    WebSocketServerNegotiation& operator=(
        WebSocketServerNegotiation&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const & noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const && = delete;

    [[nodiscard]] WebSocketDeflateNegotiation deflate() const noexcept {
        return deflate_;
    }

    [[nodiscard]] std::string_view extensions() const noexcept {
        return webSocketDeflateResponseExtensions(deflate_);
    }

private:
    friend WebSocketServerNegotiation makeWebSocketServerNegotiation(
        const HttpRequest&,
        std::string_view,
        std::pmr::memory_resource*);

    WebSocketServerNegotiation(
        std::string_view subprotocol,
        WebSocketDeflateNegotiation deflate,
        std::pmr::memory_resource* resource);

    std::pmr::string subprotocol_;
    WebSocketDeflateNegotiation deflate_;
};

[[nodiscard]] WebSocketServerNegotiation
makeWebSocketServerNegotiation(
    const HttpRequest& request,
    std::string_view supportedSubprotocols,
    std::pmr::memory_resource* resource = nullptr);

}  // namespace ruvia::detail
