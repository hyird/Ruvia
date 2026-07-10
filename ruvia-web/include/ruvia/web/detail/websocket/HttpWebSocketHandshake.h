#pragma once

#include <array>
#include <asio/write.hpp>
#include <string_view>

#include "ruvia/http/detail/websocket/HttpWebSocketServerHandshake.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/app/Task.h"

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
    const auto handshake = makeHttpWebSocketServerHandshake(request, flags, supportedSubprotocols);
    permessageDeflate = handshake.permessageDeflate;

    // Default-constructed buffers are empty (0 bytes), so the unused tail
    // entries write nothing and the present headers keep their order.
    std::array<asio::const_buffer, 10> buffers;
    std::size_t count = 0;
    buffers[count++] = asio::buffer(kHttpWebSocketSwitchingProtocolsPrefix);
    buffers[count++] = asio::buffer(handshake.accept);
    buffers[count++] = asio::buffer(kHttpCrlf);
    if (!handshake.subprotocol.empty()) {
        buffers[count++] = asio::buffer(kHttpWebSocketSubprotocolHeaderPrefix);
        buffers[count++] = asio::buffer(handshake.subprotocol);
        buffers[count++] = asio::buffer(kHttpCrlf);
    }
    if (!handshake.extensions.empty()) {
        buffers[count++] = asio::buffer(kHttpWebSocketExtensionsHeaderPrefix);
        buffers[count++] = asio::buffer(handshake.extensions);
        buffers[count++] = asio::buffer(kHttpCrlf);
    }
    buffers[count++] = asio::buffer(kHttpCrlf);
    (void)count;

    const auto ec = co_await asyncError([&stream, &buffers](auto handler) mutable {
        asio::async_write(stream, buffers, std::move(handler));
    });
    co_return !ec;
}

}  // namespace ruvia::detail
