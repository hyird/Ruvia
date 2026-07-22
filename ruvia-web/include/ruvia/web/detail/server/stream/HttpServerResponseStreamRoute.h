#pragma once

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/web/detail/server/route/Http1RouteDispatch.h"
#include "ruvia/web/detail/server/http1/Http1SessionRequestCompletion.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamDispatch.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamSink.h"
#include "ruvia/web/detail/server/response/HttpServerResponseState.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {


template <typename Stream>
Task<Http1SessionRequestCompletion> dispatchHttpResponseStreamRoute(
    Http1RouteDispatch<Stream> d,
    ResponseHeadBuffer& responseHead,
    const Http1ServerRequestHeadReady& requestHead,
    const ResolvedRoute& resolved) {
    const auto streamPlan = http1PlanResponseStream(
        d.parsed,
        d.requestSequence.nextResponseClosePolicy());
    auto connectionPlan = streamPlan.requestConnectionPlan();
    using ResponseSink = ResponseStreamSink<Stream, ConnectionScanner::Entry>;
    const auto& route = resolved.route();
    const auto& endpoint = *route.endpoint().responseStream();
    ResponseSink responseSink(
        d.stream,
        d.memory,
        responseHead,
        d.scannerEntry,
        d.baseRouteServices.worker(),
        endpoint.kind(),
        streamPlan);

    d.scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);
    auto result = co_await dispatchResponseStreamWith(
        responseSink,
        d.routes,
        d.parsed.request,
        resolved,
        d.requestMemory,
        d.baseRouteServices,
        /*peerAborted=*/[]() noexcept { return false; });

    if (result.peerAbortedBeforeCommit() != nullptr) {
        throw std::logic_error(
            "HTTP/1 d.response d.stream reported an impossible peer-abort predicate");
    }
    if (auto* recovered = result.recoveredFailure()) {
        d.response = std::move(*recovered).takeResponse();
        d.scannerEntry.touch();
        connectionPlan = requireHttp1FinalResponseCommit(
            d.response,
            streamPlan.requestConnectionPlan().requireClose());
        co_return Http1SessionRequestCompletion::makeBufferedClosing(
            connectionPlan);
    }
    if (auto* routeResponse = result.routeResponse()) {
        d.response = std::move(*routeResponse).takeResponse();
        d.scannerEntry.touch();
        connectionPlan = finalizeBufferedRouteResponse(
            d.response,
            connectionPlan,
            d.requestSequence);
        co_return Http1SessionRequestCompletion::makeBufferedUnrestored(
            connectionPlan,
            requestHead.headerBytes());
    }

    const auto committedStatus = result.committedStatus();
    if (!committedStatus.has_value()) {
        throw std::logic_error(
            "d.response d.stream dispatch returned no H1 terminal alternative");
    }

    connectionPlan = responseSink.connectionPlan();
    if (result.completed() != nullptr) {
        d.requestSequence.completeCommittedResponse(connectionPlan);
    } else {
        connectionPlan = connectionPlan.requireClose();
    }
    co_return Http1SessionRequestCompletion::makeCommittedStream(
        connectionPlan,
        *committedStatus,
        requestHead.headerBytes());
}

}  // namespace ruvia::detail
