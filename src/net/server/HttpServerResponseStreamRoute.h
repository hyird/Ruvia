#pragma once

#include "ConnectionScanner.h"
#include "HttpResponseStreamSink.h"
#include "HttpServerRequestState.h"
#include "HttpServerResponseState.h"
#include "../../http/HttpParserInternal.h"
#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <cstddef>
#include <exception>
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
    ResponseStreamWriter responseStream(
        &responseSink,
        &ResponseSink::writeThunk,
        &ResponseSink::endThunk,
        &ResponseSink::bindContextThunk,
        &ResponseSink::scratchThunk);

    std::exception_ptr exception;
    bool streamHandled = false;
    try {
        scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);
        auto result = co_await routes.dispatchResponseStream(
            parsed.request,
            routeResolution,
            requestMemory,
            responseStream,
            baseRouteServices);
        streamHandled = result.streamHandled;
        if (streamHandled || responseSink.committed()) {
            co_await responseStream.end();
        } else {
            response = std::move(result.response);
        }
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        if (responseSink.committed()) {
            co_return HttpResponseStreamRouteResult::kSessionFinished;
        }
        response = co_await routes.handleException(
            parsed.request,
            requestMemory,
            exception,
            true,
            baseRouteServices);
        keepAlive = false;
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::kWriteBufferedResponse;
    }

    if (!streamHandled && !responseSink.committed()) {
        finalizeBufferedRouteResponse(
            response,
            keepAlive,
            requestCount,
            options.maxRequestsPerConnection);
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::kWriteBufferedResponse;
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
