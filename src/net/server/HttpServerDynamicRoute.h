#pragma once

#include "../body/HttpRequestBody.h"
#include "ConnectionScanner.h"
#include "HttpResponseStreamDispatch.h"
#include "HttpResponseStreamSink.h"
#include "HttpServerBodyRouteCompletion.h"
#include "HttpServerRequestState.h"
#include "HttpServerResponseState.h"
#include "HttpServerResponseStreamRoute.h"
#include "../../http/RequestBodyLoader.h"
#include "../../http/HttpParserInternal.h"
#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <cstddef>
#include <exception>
#include <optional>
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
    RouteServices baseRouteServices,
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
    std::optional<LazyBufferedBody<Stream>> lazyBody;
    std::optional<RequestBodyLoader> bodyLoader;
    try {
        lazyBody.emplace(
            stream,
            memory.allocator<char>(),
            requestMemory.resource(),
            bodyAndPipeline,
            parsed.contentLength,
            parsed.chunked,
            parsed.transferCodings,
            options.maxBufferedBodyBytes,
            scannerEntry,
            (parsed.contentLength > 0 || parsed.chunked) && wantsContinue(parsed));
        bodyLoader.emplace(
            &*lazyBody,
            &LazyBufferedBody<Stream>::readAllThunk,
            &LazyBufferedBody<Stream>::discardThunk);
    } catch (...) {
        setupException = std::current_exception();
    }
    if (setupException != nullptr) {
        co_await completeFailedHttpBodyRoute(
            scannerEntry, setupException, parsed, routes, requestMemory, baseRouteServices, response, keepAlive);
        co_return HttpResponseStreamRouteResult::kWriteBufferedResponse;
    }

    using ResponseSink = ResponseStreamSink<Stream, ConnectionScanner::Entry>;
    ResponseSink responseSink(stream, memory, responseHead, scannerEntry, routeResolution.route->responseMode);
    scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);

    auto result = co_await dispatchResponseStreamWith(
        responseSink,
        routes,
        parsed.request,
        routeResolution,
        requestMemory,
        baseRouteServices.withBodyLoader(&*bodyLoader),
        /*closeConnectionOnError=*/true,
        /*peerAborted=*/[]() noexcept { return false; });

    switch (result.outcome) {
        case ResponseStreamDispatchOutcome::kAbortedAfterCommit:
        case ResponseStreamDispatchOutcome::kAbortedByPeer:
            co_return HttpResponseStreamRouteResult::kSessionFinished;
        case ResponseStreamDispatchOutcome::kFailedBeforeCommit:
            response = std::move(result.response);
            keepAlive = false;
            scannerEntry.touch();
            co_return HttpResponseStreamRouteResult::kWriteBufferedResponse;
        case ResponseStreamDispatchOutcome::kBuffered:
            response = std::move(result.response);
            completeSuccessfulHttpBodyRoute(
                scannerEntry,
                response,
                keepAlive,
                requestCount,
                options.maxRequestsPerConnection,
                lazyBody->consumed(),
                readBuffer,
                usedBytes,
                consumedBytes,
                bufferAlreadyCompacted,
                [&lazyBody](std::pmr::string& buffer, std::size_t& size) {
                    lazyBody->restorePipeline(buffer, size);
                });
            co_return HttpResponseStreamRouteResult::kWriteBufferedResponse;
        case ResponseStreamDispatchOutcome::kStreamed:
            break;
    }

    // Streamed: keep-alive only if the body was fully consumed, otherwise the
    // unread request body would desync the next request on this connection.
    keepAlive = keepAlive && lazyBody->consumed();
    recordCompletedRequest(keepAlive, requestCount, options.maxRequestsPerConnection);
    if (keepAlive) {
        lazyBody->restorePipeline(readBuffer, usedBytes);
        consumedBytes = 0;
        bufferAlreadyCompacted = true;
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::kStreamDispatched;
    }
    co_return HttpResponseStreamRouteResult::kSessionFinished;
}

}  // namespace ruvia::detail
