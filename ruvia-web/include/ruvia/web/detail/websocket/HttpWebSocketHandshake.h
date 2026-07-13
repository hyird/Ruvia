#pragma once

#include <array>
#include <asio/write.hpp>
#include <span>
#include <string_view>

#include "ruvia/http/detail/websocket/HttpWebSocketServerHandshake.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"

namespace ruvia::detail {

// Flush the exact HTTP-owned 101 handshake prepared by the route. Negotiation is
// not recomputed and no compression bool is returned through a side channel.
template <typename Stream>
Task<bool> writeWebSocketHandshake(
    Stream& stream,
    const HttpWebSocketServerHandshake& handshake) {
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
