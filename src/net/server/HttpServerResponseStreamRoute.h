#pragma once

#include "ConnectionScanner.h"
#include "HttpResponseStreamDispatch.h"
#include "HttpResponseStreamSink.h"
#include "HttpServerRequestState.h"
#include "HttpServerResponseState.h"
#include "../../http/HttpParserInternal.h"
#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <cstddef>
#include <utility>

namespace ruvia::detail {

enum class HttpResponseStreamRouteResult {
    kWriteBufferedResponse,
    kStreamDispatched,
    kSessionFinished
};

template <typename Stream>
Task<HttpResponseStreamRouteResult> dispatchHttpResponseStreamRoute(
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
    HttpResponse& response,
    bool& keepAlive,
    std::size_t& requestCount) {
    keepAlive = shouldKeepAlive(parsed) && parsed.contentLength == 0 && !parsed.chunked;
    using ResponseSink = ResponseStreamSink<Stream, ConnectionScanner::Entry>;
    ResponseSink responseSink(
        stream,
        memory,
        responseHead,
        scannerEntry,
        routeResolution.route->responseMode);

    scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);
    auto result = co_await dispatchResponseStreamWith(
        responseSink,
        routes,
        parsed.request,
        routeResolution,
        requestMemory,
        baseRouteServices,
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
            finalizeBufferedRouteResponse(
                response,
                keepAlive,
                requestCount,
                options.maxRequestsPerConnection);
            scannerEntry.touch();
            co_return HttpResponseStreamRouteResult::kWriteBufferedResponse;
        case ResponseStreamDispatchOutcome::kStreamed:
            break;
    }

    recordCompletedRequest(
        keepAlive,
        requestCount,
        options.maxRequestsPerConnection);
    if (!keepAlive) {
        co_return HttpResponseStreamRouteResult::kSessionFinished;
    }
    co_return HttpResponseStreamRouteResult::kStreamDispatched;
}

}  // namespace ruvia::detail
