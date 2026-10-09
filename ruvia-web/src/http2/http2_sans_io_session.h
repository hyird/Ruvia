#pragma once

#include <string_view>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/task.h"

#include "http2/http2_sans_io_session_context.h"

namespace ruvia {
class worker_memory;
}

namespace ruvia::detail {

class route_table;

// HTTP/2 supports exactly the two transports owned by the Web server. Keeping
// these concrete overloads out of headers prevents every server/test consumer
// from instantiating the complete session coroutine again.
[[nodiscard]] task<void> run_http2_sans_io_session(asio::ip::tcp::socket& stream,
    const route_table& routes_value, worker_memory& worker_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes = {});

[[nodiscard]] task<void> run_http2_sans_io_session(asio::ssl::stream<asio::ip::tcp::socket&>& stream,
    const route_table& routes_value, worker_memory& worker_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes = {});

}  // namespace ruvia::detail
