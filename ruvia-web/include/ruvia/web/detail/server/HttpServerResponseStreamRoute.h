#pragma once

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/Http1SessionRequestCompletion.h"
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
#include <stdexcept>
#include <utility>

namespace ruvia::detail {

template <typename Stream>
Task<Http1SessionRequestCompletion> dispatchHttpResponseStreamRoute(
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
    std::size_t& requestCount) {
    const auto streamPlan = http1PlanResponseStream(
        parsed,
        nextHttp1ResponseClosePolicy(requestCount, options.keepaliveRequests));
    auto connectionPlan = streamPlan.requestConnectionPlan();
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

    if (const auto* failed = result.failedAfterCommit()) {
        connectionPlan = responseSink.connectionPlan().requireClose();
        co_return Http1SessionRequestCompletion::makeCommittedStream(
            connectionPlan,
            failed->status(),
            parsed.headerBytes);
    }
    if (const auto* peer = result.peerAbortedAfterCommit()) {
        connectionPlan = responseSink.connectionPlan().requireClose();
        co_return Http1SessionRequestCompletion::makeCommittedStream(
            connectionPlan,
            peer->status(),
            parsed.headerBytes);
    }
    if (result.peerAbortedBeforeCommit() != nullptr) {
        throw std::logic_error(
            "HTTP/1 response stream reported an impossible peer-abort predicate");
    }
    if (auto* failed = result.failedBeforeCommit()) {
        response = std::move(*failed).takeResponse();
        connectionPlan = http1FinalizeResponseConnection(
            response,
            streamPlan.requestConnectionPlan().requireClose());
        scannerEntry.touch();
        co_return Http1SessionRequestCompletion::makeBufferedClosing(
            connectionPlan);
    }
    if (auto* buffered = result.buffered()) {
        response = std::move(*buffered).takeResponse();
        connectionPlan = finalizeBufferedRouteResponse(
            response,
            connectionPlan,
            requestCount,
            options.keepaliveRequests);
        scannerEntry.touch();
        co_return Http1SessionRequestCompletion::makeBufferedUnrestored(
            connectionPlan,
            parsed.headerBytes);
    }

    const auto* completed = result.completed();
    if (completed == nullptr) {
        throw std::logic_error(
            "response stream dispatch returned no H1 terminal alternative");
    }

    // The pre-commit close policy already included this completed request in the
    // plan. After bytes are committed, the prepared sink disposition is the only
    // lifecycle verdict; recomputing the limit here could close without having sent
    // the matching Connection signal.
    ++requestCount;
    connectionPlan = responseSink.connectionPlan();
    co_return Http1SessionRequestCompletion::makeCommittedStream(
        connectionPlan,
        completed->status(),
        parsed.headerBytes);
}

}  // namespace ruvia::detail
