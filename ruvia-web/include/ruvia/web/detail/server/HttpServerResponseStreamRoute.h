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
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>

namespace ruvia::detail {

class HttpResponseStreamBufferedRoute final {
private:
    friend class HttpResponseStreamRouteResult;

    constexpr HttpResponseStreamBufferedRoute() noexcept = default;
};

class HttpResponseStreamCommittedRoute final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class HttpResponseStreamRouteResult;

    explicit constexpr HttpResponseStreamCommittedRoute(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

// The H1 route either leaves one buffered response for the session writer or has
// already committed a stream head with the exact wire status. Connection reuse is
// carried separately by the HTTP-owned connection plan output.
class HttpResponseStreamRouteResult final {
public:
    [[nodiscard]] static constexpr HttpResponseStreamRouteResult
    makeBuffered() noexcept {
        return HttpResponseStreamRouteResult(
            HttpResponseStreamBufferedRoute{});
    }

    [[nodiscard]] static constexpr HttpResponseStreamRouteResult
    makeCommitted(std::uint16_t status) noexcept {
        return HttpResponseStreamRouteResult(
            HttpResponseStreamCommittedRoute(status));
    }

    [[nodiscard]] constexpr const HttpResponseStreamBufferedRoute*
    buffered() const noexcept {
        return std::get_if<HttpResponseStreamBufferedRoute>(&value_);
    }

    [[nodiscard]] constexpr const HttpResponseStreamCommittedRoute*
    committed() const noexcept {
        return std::get_if<HttpResponseStreamCommittedRoute>(&value_);
    }

private:
    using Value = std::variant<
        HttpResponseStreamBufferedRoute,
        HttpResponseStreamCommittedRoute>;

    template <typename Alternative>
    explicit constexpr HttpResponseStreamRouteResult(
        Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
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

    if (const auto* failed = result.failedAfterCommit()) {
        connectionPlan = responseSink.connectionPlan().requireClose();
        co_return HttpResponseStreamRouteResult::makeCommitted(
            failed->status());
    }
    if (const auto* peer = result.peerAbortedAfterCommit()) {
        connectionPlan = responseSink.connectionPlan().requireClose();
        co_return HttpResponseStreamRouteResult::makeCommitted(
            peer->status());
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
        co_return HttpResponseStreamRouteResult::makeBuffered();
    }
    if (auto* buffered = result.buffered()) {
        response = std::move(*buffered).takeResponse();
        connectionPlan = finalizeBufferedRouteResponse(
            response,
            connectionPlan,
            requestCount,
            options.keepaliveRequests);
        scannerEntry.touch();
        co_return HttpResponseStreamRouteResult::makeBuffered();
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
    co_return HttpResponseStreamRouteResult::makeCommitted(
        completed->status());
}

}  // namespace ruvia::detail
