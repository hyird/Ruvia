#pragma once

#include <string_view>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"

#include "http2/http2_sans_io_session_context.h"
#include "server/http_connection_close.h"

namespace ruvia {
class worker_memory;
}

namespace ruvia::detail {

class route_table;

// HTTP/2 supports exactly the two transports owned by the Web server. Keeping
// these concrete overloads out of headers prevents every server/test consumer
// from instantiating the complete session coroutine again. The result is
// notify when the peer ended its input cleanly, linger when the session ended
// with GOAWAY (a completed drain or a connection error), otherwise abort.
[[nodiscard]] task<http_connection_close> run_http2_sans_io_session(asio::ip::tcp::socket& stream,
    const route_table& routes_value, worker_memory& worker_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes = {});

[[nodiscard]] task<http_connection_close> run_http2_sans_io_session(server_tls_stream& stream,
    const route_table& routes_value, worker_memory& worker_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes = {});

}  // namespace ruvia::detail
