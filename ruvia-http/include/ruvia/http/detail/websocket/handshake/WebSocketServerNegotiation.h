#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeFields.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"

namespace ruvia::detail {

struct WebSocketServerNegotiationOptions final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    std::span<const std::string_view> supportedSubprotocols{};
    std::span<const HttpHeaderView> responseHeaders{};
    std::pmr::memory_resource* resource{nullptr};
    WebSocketDeflateConfig deflate{};
};

// HTTP-version-independent result of one server-side WebSocket negotiation. HTTP/1
// and RFC 8441 consume this same immutable value for their response head, and the
// subsequent WsConnection consumes its exact deflate alternative. This prevents
// response metadata and frame RSV1 semantics from being configured separately.
// The selected subprotocol is copied into caller-selected PMR storage so this
// committed value never borrows mutable route configuration.
class WebSocketServerNegotiation final {
public:
    WebSocketServerNegotiation(const WebSocketServerNegotiation&) = delete;
    WebSocketServerNegotiation& operator=(const WebSocketServerNegotiation&) = delete;
    WebSocketServerNegotiation(WebSocketServerNegotiation&&) noexcept = default;
    WebSocketServerNegotiation& operator=(WebSocketServerNegotiation&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] WebSocketCompression compression() const noexcept {
        return compression_;
    }

    [[nodiscard]] std::string_view extensions() const noexcept {
        return webSocketCompressionExtension(compression_);
    }

    [[nodiscard]] std::span<const HttpHeader> responseHeaders() const& noexcept {
        return responseHeaders_;
    }
    std::span<const HttpHeader> responseHeaders() const&& = delete;

private:
    friend WebSocketServerNegotiation makeWebSocketServerNegotiation(
        const HttpRequest&, WebSocketServerNegotiationOptions);

    WebSocketServerNegotiation(std::string_view subprotocol, WebSocketCompression compression,
        std::span<const HttpHeaderView> responseHeaders, std::pmr::memory_resource* resource);

    std::pmr::string subprotocol_;
    WebSocketCompression compression_;
    std::pmr::vector<HttpHeader> responseHeaders_;
};

[[nodiscard]] WebSocketServerNegotiation makeWebSocketServerNegotiation(
    const HttpRequest& request, WebSocketServerNegotiationOptions options = {});

}  // namespace ruvia::detail
