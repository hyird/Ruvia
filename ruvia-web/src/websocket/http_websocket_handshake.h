#pragma once

#include <array>
#include <span>
#include <string_view>
#include <system_error>

#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/http/websocket_handshake.h"

namespace ruvia::detail {

// Flush the exact HTTP-owned 101 handshake prepared by the route. Negotiation is
// not recomputed and no compression bool is returned through a side channel.
template <typename stream_type>
task<std::error_code> write_websocket_handshake(
    stream_type& stream, const websocket_server_handshake& handshake) {
    std::array<asio::const_buffer, 10 + 4 * max_http_header_fields> buffers;
    std::size_t count = 0;
    handshake.for_each_response_part(
        [&buffers, &count](std::string_view part) { buffers[count++] = asio::buffer(part); });
    const auto active_buffers = std::span<const asio::const_buffer>(buffers.data(), count);

    const auto write_completion = co_await ruvia::async_asio([&stream, active_buffers](auto handler) mutable {
        asio::async_write(stream, active_buffers, std::move(handler));
    });
    co_return write_completion.error_code();
}

}  // namespace ruvia::detail
