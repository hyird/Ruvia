#pragma once

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/core/memory/MemoryPool.h"

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
    const Http1ServerRequestParseState& parsed,
    const RouteResolution& routeResolution,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    ContextServices baseRouteServices,
    const HttpServerOptions& options,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    HttpResponse& response,
    Http1ServerConnectionPlan& connectionPlan,
    std::size_t& requestCount,
    std::size_t& consumedBytes,
    bool& bufferAlreadyCompacted) {
    const auto bodyAndPipeline = beginHttpBodyRoute(
        parsed,
        readBuffer,
        usedBytes,
        connectionPlan,
        consumedBytes);

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
        connectionPlan = co_await completeFailedHttpBodyRoute(
            scannerEntry,
            setupException,
            parsed,
            routes,
            requestMemory,
            baseRouteServices,
            response);
        co_return;
    }

    response = co_await routes.dispatchBuffered(
        parsed.request,
        routeResolution,
        requestMemory,
        bodyState.withLoader(baseRouteServices));

    connectionPlan = completeSuccessfulHttpBodyRoute(
        scannerEntry,
        response,
        connectionPlan,
        requestCount,
        options.keepaliveRequests,
        bodyState.consumption(),
        readBuffer,
        usedBytes,
        consumedBytes,
        bufferAlreadyCompacted,
        [&bodyState](std::pmr::string& buffer, std::size_t& size) {
            bodyState.restorePipeline(buffer, size);
        });
}

}  // namespace ruvia::detail
