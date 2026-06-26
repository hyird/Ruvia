#pragma once

#include "ConnectionScanner.h"
#include "HttpResponseStreamDispatch.h"
#include "HttpResponseStreamSink.h"
#include "HttpServerBodyRouteCompletion.h"
#include "HttpServerRequestState.h"
#include "HttpServerResponseState.h"
#include "HttpServerResponseStreamRoute.h"
#include "../../http/HttpParserInternal.h"
#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <cstddef>
#include <exception>
#include <utility>

namespace ruvia::detail {

// kDynamic route dispatch: the buffered request body is set up exactly as for a
// buffered route, but a response sink is also bound so the handler may stream.
// The shared response-stream driver runs the buffered handler chain and reports
// whether it streamed (committed) or returned a buffered response; either way the
// request body is consumed/restored like a buffered route so keep-alive and
// pipelining stay correct. Reused by HTTP/1.1; HTTP/2 has its own parallel.
template <typename Stream>
Task<HttpResponseStreamRouteResult> dispatchHttpDynamicRoute(
    Stream& stream,
    WorkerMemory& memory,
    ResponseHeadBuffer& responseHead,
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
            scannerEntry, setupException, parsed, routes, requestMemory, baseRouteServices, response, keepAlive);
        co_return HttpResponseStreamRouteResult::writeBufferedResponse();
    }

    using ResponseSink = ResponseStreamSink<Stream, ConnectionScanner::Entry>;
    const auto& route = routeResolution.route();
    ResponseSink responseSink(stream, memory, responseHead, scannerEntry, route.responseMode());
    scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);

    auto result = co_await dispatchResponseStreamWith(
        responseSink,
        routes,
        parsed.request,
        routeResolution,
        requestMemory,
        bodyState.withLoader(baseRouteServices),
        /*closeConnectionOnError=*/true,
        /*peerAborted=*/[]() noexcept { return false; });

    if (result.aborted()) {
        co_return HttpResponseStreamRouteResult::sessionFinished();
    }
    if (result.failedBeforeCommit()) {
        response = result.takeResponse();
        keepAlive = false;
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::writeBufferedResponse();
    }
    if (result.buffered()) {
        response = result.takeResponse();
        completeSuccessfulHttpBodyRoute(
            scannerEntry,
            response,
            keepAlive,
            requestCount,
            options.maxRequestsPerConnection,
            bodyState.consumed(),
            readBuffer,
            usedBytes,
            consumedBytes,
            bufferAlreadyCompacted,
            [&bodyState](std::pmr::string& buffer, std::size_t& size) {
                bodyState.restorePipeline(buffer, size);
            });
        co_return HttpResponseStreamRouteResult::writeBufferedResponse();
    }

    // Streamed: keep-alive only if the body was fully consumed, otherwise the
    // unread request body would desync the next request on this connection.
    keepAlive = keepAlive && bodyState.consumed();
    recordCompletedRequest(keepAlive, requestCount, options.maxRequestsPerConnection);
    if (keepAlive) {
        bodyState.restorePipeline(readBuffer, usedBytes);
        consumedBytes = 0;
        bufferAlreadyCompacted = true;
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::streamDispatched();
    }
    co_return HttpResponseStreamRouteResult::sessionFinished();
}

}  // namespace ruvia::detail
