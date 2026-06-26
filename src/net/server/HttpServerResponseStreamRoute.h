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

class HttpResponseStreamRouteResult final {
public:
    [[nodiscard]] static constexpr HttpResponseStreamRouteResult writeBufferedResponse() noexcept {
        return HttpResponseStreamRouteResult(Outcome::kWriteBufferedResponse);
    }

    [[nodiscard]] static constexpr HttpResponseStreamRouteResult streamDispatched() noexcept {
        return HttpResponseStreamRouteResult(Outcome::kStreamDispatched);
    }

    [[nodiscard]] static constexpr HttpResponseStreamRouteResult sessionFinished() noexcept {
        return HttpResponseStreamRouteResult(Outcome::kSessionFinished);
    }

    [[nodiscard]] constexpr bool didDispatchStream() const noexcept {
        return outcome_ == Outcome::kStreamDispatched;
    }

    [[nodiscard]] constexpr bool finishedSession() const noexcept {
        return outcome_ == Outcome::kSessionFinished;
    }

private:
    enum class Outcome {
        kWriteBufferedResponse,
        kStreamDispatched,
        kSessionFinished
    };

    explicit constexpr HttpResponseStreamRouteResult(Outcome outcome) noexcept
        : outcome_(outcome) {}

    Outcome outcome_;
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
    ContextServices baseRouteServices,
    const HttpServerOptions& options,
    HttpResponse& response,
    bool& keepAlive,
    std::size_t& requestCount) {
    keepAlive = shouldKeepAlive(parsed) && parsed.contentLength == 0 && !parsed.chunked;
    using ResponseSink = ResponseStreamSink<Stream, ConnectionScanner::Entry>;
    const auto& route = routeResolution.route();
    ResponseSink responseSink(
        stream,
        memory,
        responseHead,
        scannerEntry,
        route.responseMode());

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
        finalizeBufferedRouteResponse(
            response,
            keepAlive,
            requestCount,
            options.maxRequestsPerConnection);
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::writeBufferedResponse();
    }

    recordCompletedRequest(
        keepAlive,
        requestCount,
        options.maxRequestsPerConnection);
    if (!keepAlive) {
        co_return HttpResponseStreamRouteResult::sessionFinished();
    }
    co_return HttpResponseStreamRouteResult::streamDispatched();
}

}  // namespace ruvia::detail
