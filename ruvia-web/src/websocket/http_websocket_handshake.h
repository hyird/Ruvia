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
    std::array<char, max_http_header_bytes> head_bytes;
    std::size_t size = 0;
    handshake.for_each_response_part([&head_bytes, &size](std::string_view part) {
        std::char_traits<char>::copy(head_bytes.data() + size, part.data(), part.size());
        size += part.size();
    });
    const auto active_buffer = asio::buffer(head_bytes.data(), size);

    const auto write_completion = co_await ruvia::async_asio([&stream, active_buffer](auto handler) mutable {
        asio::async_write(stream, active_buffer, std::move(handler));
    });
    co_return write_completion.error_code();
}

}  // namespace ruvia::detail
