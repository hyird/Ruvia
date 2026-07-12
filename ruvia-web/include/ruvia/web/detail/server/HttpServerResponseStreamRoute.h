#pragma once

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpResponseStreamDispatch.h"
#include "ruvia/web/detail/server/HttpResponseStreamSink.h"
#include "ruvia/web/detail/server/HttpServerRequestState.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
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
    const Http1ServerRequestParseState& parsed,
    const ResolvedRoute& resolved,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    ContextServices baseRouteServices,
    const HttpServerOptions& options,
    HttpResponse& response,
    Http1ServerConnectionPlan& connectionPlan,
    std::size_t& requestCount) {
    const auto streamPlan = http1PlanResponseStream(
        parsed,
        nextHttp1ResponseClosePolicy(requestCount, options.keepaliveRequests));
    connectionPlan = streamPlan.requestConnectionPlan();
    using ResponseSink = ResponseStreamSink<Stream, ConnectionScanner::Entry>;
    const auto& route = resolved.route();
    const auto& endpoint = *route.endpoint().responseStream();
    ResponseSink responseSink(
        stream,
        memory,
        responseHead,
        scannerEntry,
        endpoint.kind(),
        streamPlan);

    scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);
    auto result = co_await dispatchResponseStreamWith(
        responseSink,
        routes,
        parsed.request,
        resolved,
        requestMemory,
        baseRouteServices,
        /*peerAborted=*/[]() noexcept { return false; });

    if (result.aborted()) {
        co_return HttpResponseStreamRouteResult::sessionFinished();
    }
    if (result.failedBeforeCommit()) {
        response = result.takeResponse();
        connectionPlan = http1FinalizeResponseConnection(
            response,
            streamPlan.requestConnectionPlan().requireClose());
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::writeBufferedResponse();
    }
    if (result.buffered()) {
        response = result.takeResponse();
        connectionPlan = finalizeBufferedRouteResponse(
            response,
            connectionPlan,
            requestCount,
            options.keepaliveRequests);
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::writeBufferedResponse();
    }

    // The pre-commit close policy already included this completed request in the
    // plan. After bytes are committed, the prepared sink disposition is the only
    // lifecycle verdict; recomputing the limit here could close without having sent
    // the matching Connection signal.
    ++requestCount;
    connectionPlan = responseSink.connectionPlan();
    if (connectionPlan.disposition() == Http1ConnectionDisposition::kClose) {
        co_return HttpResponseStreamRouteResult::sessionFinished();
    }
    co_return HttpResponseStreamRouteResult::streamDispatched();
}

}  // namespace ruvia::detail
