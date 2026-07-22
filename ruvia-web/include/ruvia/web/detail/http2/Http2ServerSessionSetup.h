#pragma once

#include <asio/ip/tcp.hpp>

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerWorkerState.h"

// What starting an HTTP/2 server session takes from the connection that hands
// it over: the stream to speak on, the socket underneath it, the worker's
// memory and scanner entry, and the routes, options, services and worker state
// every request on the session will be served with. All of it is per
// connection, so it is gathered once where the connection is accepted rather
// than threaded through each entry point.

namespace ruvia::detail {

template <typename Stream>
struct Http2ServerSessionSetup final {
    Stream& stream;
    asio::ip::tcp::socket& socket;
    WorkerMemory& memory;
    const RouteTable& routes;
    const HttpServerOptions& options;
    ConnectionScanner::Entry& scannerEntry;
    ContextServices services;
    const HttpServerWorkerState& workerState;
};

}  // namespace ruvia::detail
