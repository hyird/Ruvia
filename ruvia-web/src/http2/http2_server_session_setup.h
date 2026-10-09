#pragma once

#include <asio/ip/tcp.hpp>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"

#include "context/context_services.h"
#include "router/route_table.h"
#include "server/http_server_options.h"
#include "server/http_server_worker_state.h"

// What starting an HTTP/2 server session takes from the connection that hands
// it over: the stream to speak on, the socket underneath it, the worker's
// memory and scanner entry, and the routes, options, services and worker state
// every request on the session will be served with. All of it is per
// connection, so it is gathered once where the connection is accepted rather
// than threaded through each entry point.

namespace ruvia::detail {

template <typename stream_type>
struct http2_server_session_setup final {
    stream_type& stream_;
    asio::ip::tcp::socket& socket_;
    worker_memory& memory_;
    const route_table& routes_;
    const http_server_options& options_;
    ruvia::connection_scanner::entry_type& scanner_entry_;
    context_services services_;
    const http_server_worker_state& worker_state_;
};

}  // namespace ruvia::detail
