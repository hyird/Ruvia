#pragma once

#include <array>
#include <asio/write.hpp>
#include <string_view>

#include "HttpWebSocketPermessageDeflate.h"
#include "HttpWebSocketUtils.h"
#include "../../http/HttpRequestFlags.h"
#include "../../http/HttpRequestInternal.h"
#include "../../runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

// Writes the 101 handshake. `permessageDeflate` is set to whether the RFC 7692
// extension was negotiated (offered by the client and acceptable to us); when
// set, the response advertises it with no-context-takeover in both directions.
template <typename Stream>
Task<bool> writeWebSocketHandshake(
    Stream& stream,
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view supportedSubprotocols,
    bool& permessageDeflate) {
    const auto subprotocol = chooseWebSocketSubprotocol(request, flags, supportedSubprotocols);
    const auto deflate = webSocketNegotiatePermessageDeflate(request);
    permessageDeflate = deflate.enabled;
    static constexpr std::string_view kPrefix =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    static constexpr std::string_view kSubprotocolPrefix = "Sec-WebSocket-Protocol: ";
    static constexpr std::string_view kExtensionsHeader =
        "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n";
    // Same offer, but the client pinned server_max_window_bits=15; RFC 7692
    // §7.1.2.1 requires echoing the accepted value in the response.
    static constexpr std::string_view kExtensionsHeaderMaxWindow =
        "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; "
        "client_no_context_takeover; server_max_window_bits=15\r\n";
    static constexpr std::string_view kCrlf = "\r\n";

    WebSocketAcceptKey accept;
    encodeWebSocketAccept(accept, requestKnownHeader(request, RequestKnownHeader::kSecWebSocketKey));

    // Default-constructed buffers are empty (0 bytes), so the unused tail
    // entries write nothing and the present headers keep their order.
    std::array<asio::const_buffer, 8> buffers;
    std::size_t count = 0;
    buffers[count++] = asio::buffer(kPrefix);
    buffers[count++] = asio::buffer(accept);
    buffers[count++] = asio::buffer(kCrlf);
    if (!subprotocol.empty()) {
        buffers[count++] = asio::buffer(kSubprotocolPrefix);
        buffers[count++] = asio::buffer(subprotocol);
        buffers[count++] = asio::buffer(kCrlf);
    }
    if (permessageDeflate) {
        buffers[count++] = asio::buffer(
            deflate.echoServerMaxWindowBits ? kExtensionsHeaderMaxWindow : kExtensionsHeader);
    }
    buffers[count++] = asio::buffer(kCrlf);
    (void)count;

    const auto ec = co_await asyncError([&stream, &buffers](auto handler) mutable {
        asio::async_write(stream, buffers, std::move(handler));
    });
    co_return !ec;
}

}  // namespace ruvia::detail
