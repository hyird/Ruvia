#pragma once

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpResponseStreamDispatch.h"
#include "ruvia/web/detail/server/HttpResponseStreamSink.h"
#include "ruvia/web/detail/server/HttpServerRequestState.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/http/detail/HttpParserInternal.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/core/memory/MemoryPool.h"

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
    const auto streamPlan = http1PlanResponseStream(
        parsed,
        requestLimitReached(requestCount + 1, options.keepaliveRequests));
    keepAlive = streamPlan.requestCanPersist();
    using ResponseSink = ResponseStreamSink<Stream, ConnectionScanner::Entry>;
    const auto& route = routeResolution.route();
    ResponseSink responseSink(
        stream,
        memory,
        responseHead,
        scannerEntry,
        route.responseMode(),
        streamPlan);

    scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);
    auto result = co_await dispatchResponseStreamWith(
        responseSink,
        routes,
        parsed.request,
        routeResolution,
        requestMemory,
        baseRouteServices,
        /*peerAborted=*/[]() noexcept { return false; });

    if (result.aborted()) {
        co_return HttpResponseStreamRouteResult::sessionFinished();
    }
    if (result.failedBeforeCommit()) {
        response = result.takeResponse();
        keepAlive = false;
        http1MarkConnectionClose(response);
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::writeBufferedResponse();
    }
    if (result.buffered()) {
        response = result.takeResponse();
        finalizeBufferedRouteResponse(
            response,
            keepAlive,
            requestCount,
            options.keepaliveRequests,
            streamPlan.needsKeepAliveSignal());
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::writeBufferedResponse();
    }

    if (streamPlan.connectionWillClose()) {
        keepAlive = false;
    }
    recordCompletedRequest(
        keepAlive,
        requestCount,
        options.keepaliveRequests);
    if (!keepAlive) {
        co_return HttpResponseStreamRouteResult::sessionFinished();
    }
    co_return HttpResponseStreamRouteResult::streamDispatched();
}

}  // namespace ruvia::detail
