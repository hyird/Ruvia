#pragma once

#include <memory_resource>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http1ServerRequestParser.h"
#include "ruvia/http/HttpAcceptEncoding.h"
#include "ruvia/http/HttpResponse.h"

#include "context/ContextServices.h"
#include "router/RouteTable.h"
#include "server/Http1RequestSequence.h"
#include "server/HttpResponseCompression.h"
#include "server/HttpServerOptions.h"
#include "server/inbound_buffer_resource.h"

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
    ruvia::ConnectionScanner::Entry& scannerEntry;
    const Http1ServerRequestParseState& parsed;
    HttpResponseCodingSelection responseCoding;
    HttpResponseCodingAvailability responseCodingAvailability;
    const RouteTable& routes;
    RequestMemory& requestMemory;
    ContextServices baseRouteServices;
    const HttpServerOptions& options;
    HttpResponse& response;
    Http1RequestSequence& requestSequence;
    std::pmr::memory_resource* inbound_buffer_pool{};
};

}  // namespace ruvia::detail
