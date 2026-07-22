#pragma once

#include <array>
#include <asio/write.hpp>
#include <span>
#include <string_view>
#include <system_error>

#include "ruvia/http/detail/websocket/handshake/HttpWebSocketServerHandshake.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/Task.h"

namespace ruvia::detail {

// Flush the exact HTTP-owned 101 handshake prepared by the route. Negotiation is
// not recomputed and no compression bool is returned through a side channel.
template <typename Stream>
Task<std::error_code> writeWebSocketHandshake(
    Stream& stream,
    const HttpWebSocketServerHandshake& handshake) {
    std::array<asio::const_buffer, 10> buffers;
    std::size_t count = 0;
    handshake.forEachResponsePart([&buffers, &count](std::string_view part) {
        buffers[count++] = asio::buffer(part);
    });
    const auto activeBuffers = std::span<const asio::const_buffer>(buffers.data(), count);

    const auto writeCompletion = co_await asyncAsio(
        [&stream, activeBuffers](auto handler) mutable {
            asio::async_write(stream, activeBuffers, std::move(handler));
        });
    co_return writeCompletion.errorCode();
}

}  // namespace ruvia::detail
