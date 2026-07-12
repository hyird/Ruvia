#pragma once

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/Http1SessionRequestCompletion.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
#include "ruvia/web/detail/websocket/HttpWebSocketHandshake.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <string_view>
#include <utility>
#include <variant>

namespace ruvia::detail {

class HttpWebSocketBufferedResponse final {
public:
    [[nodiscard]] const Http1SessionRequestCompletion&
    completion() const noexcept {
        return completion_;
    }

private:
    friend class HttpWebSocketRouteResult;

    explicit HttpWebSocketBufferedResponse(
        Http1SessionRequestCompletion completion) noexcept
        : completion_(std::move(completion)) {}

    Http1SessionRequestCompletion completion_;
};

class HttpWebSocketSessionFinished final {
private:
    friend class HttpWebSocketRouteResult;

    constexpr HttpWebSocketSessionFinished() noexcept = default;
};

class HttpWebSocketRouteResult final {
public:
    [[nodiscard]] static HttpWebSocketRouteResult makeBuffered(
        Http1SessionRequestCompletion completion) noexcept {
        return HttpWebSocketRouteResult(
            HttpWebSocketBufferedResponse(std::move(completion)));
    }

    [[nodiscard]] static HttpWebSocketRouteResult
    makeSessionFinished() noexcept {
        return HttpWebSocketRouteResult(
            HttpWebSocketSessionFinished{});
    }

    [[nodiscard]] const HttpWebSocketBufferedResponse*
    bufferedResponse() const noexcept {
        return std::get_if<HttpWebSocketBufferedResponse>(&value_);
    }

    [[nodiscard]] const HttpWebSocketSessionFinished*
    sessionFinished() const noexcept {
        return std::get_if<HttpWebSocketSessionFinished>(&value_);
    }

private:
    using Value = std::variant<
        HttpWebSocketBufferedResponse,
        HttpWebSocketSessionFinished>;

    template <typename Alternative>
    explicit HttpWebSocketRouteResult(Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

template <typename Stream>
Task<HttpWebSocketRouteResult> dispatchHttpWebSocketRoute(
    Stream& stream,
    WorkerMemory& memory,
    ConnectionScanner::Entry& scannerEntry,
    const Http1ServerRequestParseState& parsed,
    const ResolvedRoute& resolved,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    ContextServices baseRouteServices,
    const HttpServerOptions& options,
    std::string_view pendingFrames,
    HttpResponse& response) {
    if (!isValidWebSocketRequest(parsed.request) ||
        parsed.bodyPlan.requiresConsumption()) {
        response = co_await routes.handleError(
            parsed.request,
            requestMemory,
            HttpErrorInfo(400, {}, "invalid websocket upgrade"),
            baseRouteServices);
        const auto connectionPlan = http1FinalizeResponseConnection(
            response, parsed.connectionPlan.requireClose());
        co_return HttpWebSocketRouteResult::makeBuffered(
            Http1SessionRequestCompletion::makeBufferedClosing(
                connectionPlan));
    }
    const auto& webSocketEndpoint =
        *resolved.route().endpoint().webSocket();
    const auto handshake = makeHttpWebSocketServerHandshake(
        parsed.request,
        webSocketEndpoint.subprotocols());
    if (!(co_await writeWebSocketHandshake(
            stream,
            handshake))) {
        co_return HttpWebSocketRouteResult::makeSessionFinished();
    }

    SocketWebSocketConnection<Stream> webSocketConnection(
        WebSocketSocketTransport<Stream>{stream},
        scannerEntry,
        webSocketEndpoint.lifecycle(),
        options.maxWebSocketMessageBytes,
        memory.resource(),
        pendingFrames,
        handshake.negotiation().deflate());
    co_await runWebSocketSession(
        webSocketConnection,
        scannerEntry,
        routes,
        parsed.request,
        resolved,
        requestMemory,
        baseRouteServices);
    co_return HttpWebSocketRouteResult::makeSessionFinished();
}

}  // namespace ruvia::detail
