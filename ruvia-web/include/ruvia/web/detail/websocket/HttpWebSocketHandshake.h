#pragma once

#include <array>
#include <asio/write.hpp>
#include <span>
#include <string_view>

#include "ruvia/http/detail/websocket/HttpWebSocketServerHandshake.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"

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

    std::array<asio::const_buffer, 10> buffers;
    std::size_t count = 0;
    handshake.forEachResponsePart([&buffers, &count](std::string_view part) {
        buffers[count++] = asio::buffer(part);
    });
    const auto activeBuffers = std::span<const asio::const_buffer>(buffers.data(), count);

    const auto ec = co_await asyncError([&stream, activeBuffers](auto handler) mutable {
        asio::async_write(stream, activeBuffers, std::move(handler));
    });
    co_return !ec;
}

}  // namespace ruvia::detail
