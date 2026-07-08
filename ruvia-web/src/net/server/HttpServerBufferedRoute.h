#pragma once

#include "net/server/ConnectionScanner.h"
#include "HttpServerBodyRouteCompletion.h"
#include "HttpParserInternal.h"
#include "router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

template <typename Stream>
Task<void> dispatchHttpBufferedBodyRoute(
    Stream& stream,
    WorkerMemory& memory,
    ConnectionScanner::Entry& scannerEntry,
    const HttpServerParseResult& parsed,
    const RouteResolution& routeResolution,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    ContextServices baseRouteServices,
    const HttpServerOptions& options,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    HttpResponse& response,
    bool& keepAlive,
    std::size_t& requestCount,
    std::size_t& consumedBytes,
    bool& bufferAlreadyCompacted) {
    const auto bodyAndPipeline = beginHttpBodyRoute(parsed, readBuffer, usedBytes, keepAlive, consumedBytes);

    // The body reader/loader are this transport's own state, and their setup can
    // throw (e.g. constructing a transfer-coding decoder for a bad
    // Transfer-Encoding), so it stays guarded here. The dispatch itself is the
    // routing layer's concern and never throws: dispatchBuffered turns any
    // handler or routing failure into a response, so it sits outside the guard.
    std::exception_ptr setupException;
    HttpLazyBufferedBodyRouteState<Stream> bodyState;
    try {
        prepareHttpLazyBufferedBodyRoute(
            bodyState,
            stream,
            memory,
            requestMemory,
            bodyAndPipeline,
            parsed,
            options,
            scannerEntry);
    } catch (...) {
        setupException = std::current_exception();
    }

    if (setupException != nullptr) {
        co_await completeFailedHttpBodyRoute(
            scannerEntry,
            setupException,
            parsed,
            routes,
            requestMemory,
            baseRouteServices,
            response,
            keepAlive);
        co_return;
    }

    response = co_await routes.dispatchBuffered(
        parsed.request,
        routeResolution,
        requestMemory,
        true,
        bodyState.withLoader(baseRouteServices));

    completeSuccessfulHttpBodyRoute(
        scannerEntry,
        response,
        keepAlive,
        requestCount,
        options.keepaliveRequests,
        bodyState.consumed(),
        requestNeedsKeepAliveSignal(parsed.request.httpVersion()),
        readBuffer,
        usedBytes,
        consumedBytes,
        bufferAlreadyCompacted,
        [&bodyState](std::pmr::string& buffer, std::size_t& size) {
            bodyState.restorePipeline(buffer, size);
        });
}

}  // namespace ruvia::detail
