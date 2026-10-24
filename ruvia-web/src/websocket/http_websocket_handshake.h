#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <system_error>

#include "ruvia/core/task.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/websocket_handshake.h"

#include "server/http1_upgrade_response_head.h"

namespace ruvia::detail {

// Flush the exact HTTP-owned 101 handshake prepared by the route. Negotiation is
// not recomputed and no compression bool is returned through a side channel.
// The shared HTTP/1 upgrade-head writer applies the request `expectations`
// (100 Continue before 101, RFC 9110 section 7.8).
template <typename stream_type>
task<std::error_code> write_websocket_handshake(stream_type& stream, const websocket_server_handshake& handshake,
    const http_request_expectations& expectations) {
    std::array<char, max_http_header_bytes> head_bytes;
    std::size_t size = 0;
    handshake.for_each_response_part([&head_bytes, &size](std::string_view part) {
        std::char_traits<char>::copy(head_bytes.data() + size, part.data(), part.size());
        size += part.size();
    });
    co_return co_await write_http1_upgrade_response_head(
        stream, expectations, http_status::switching_protocols, std::string_view(head_bytes.data(), size));
}

}  // namespace ruvia::detail
