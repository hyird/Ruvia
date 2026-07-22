#pragma once

#include <memory_resource>

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/http1/Http1RequestSequence.h"

// What every HTTP/1 route dispatch needs from the session that owns the
// request: the transport, the worker's memory and scanner entry, the parsed
// head, the route table and services to run the handler with, the options, the
// response being built, and the keep-alive sequence. Each dispatcher takes this
// and adds only what its own kind of route needs.
//
// Passed by value, as its members were as separate arguments: the references
// cost nothing to copy and ContextServices is copied per dispatch either way.

namespace ruvia::detail {

template <typename Stream>
struct Http1RouteDispatch final {
    Stream& stream;
    WorkerMemory& memory;
    ConnectionScanner::Entry& scannerEntry;
    const Http1ServerRequestParseState& parsed;
    const RouteTable& routes;
    RequestMemory& requestMemory;
    ContextServices baseRouteServices;
    const HttpServerOptions& options;
    HttpResponse& response;
    Http1RequestSequence& requestSequence;
};

}  // namespace ruvia::detail
