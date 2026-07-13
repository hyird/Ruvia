#pragma once

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

template <typename Stream>
Task<Http1SessionRequestCompletion> dispatchHttpBufferedBodyRoute(
    Stream& stream,
    WorkerMemory& memory,
    ConnectionScanner::Entry& scannerEntry,
    const Http1ServerRequestParseState& parsed,
    const Http1ServerRequestHeadReady& requestHead,
    const RouteResolution& routeResolution,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    ContextServices baseRouteServices,
    const HttpServerOptions& options,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    HttpResponse& response,
    Http1RequestSequence& requestSequence) {
    const auto bodyAndPipeline = httpBodyAndPipeline(
        requestHead,
        readBuffer,
        usedBytes);

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
        co_return co_await completeFailedHttpBodyRoute(
            scannerEntry,
            setupException,
            parsed,
            routes,
            requestMemory,
            baseRouteServices,
            response);
    }

    response = co_await routes.dispatchBuffered(
        parsed.request,
        routeResolution,
        requestMemory,
        bodyState.withLoader(baseRouteServices));

    co_return completeSuccessfulHttpBodyRoute(
        scannerEntry,
        response,
        parsed.connectionPlan,
        requestSequence,
        bodyState.consumption(),
        readBuffer,
        usedBytes,
        [&bodyState](std::pmr::string& buffer, std::size_t& size) {
            bodyState.restorePipeline(buffer, size);
        });
}

}  // namespace ruvia::detail
