#pragma once

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/Http1SessionRequestCompletion.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
#include "ruvia/web/detail/websocket/HttpWebSocketHandshake.h"
#include "ruvia/web/detail/http/HttpProtocolErrorInfo.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/detail/websocket/HttpWebSocketHandshakeValidation.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <exception>
#include <optional>
#include <string_view>

namespace ruvia::detail {

// A rejected upgrade returns the exact HTTP/1 request completion that the
// session must write and clean up. A successful upgrade transfers transport
// ownership to the WebSocket session, so no HTTP request completion remains.
template <typename Stream>
Task<std::optional<Http1SessionRequestCompletion>> dispatchHttpWebSocketRoute(
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
    const auto handshakeValidation = validateHttp1WebSocketHandshake(
        parsed.request,
        parsed.bodyPlan);
    if (const auto* failure = handshakeValidation.failure()) {
        response = co_await routes.handleError(
            parsed.request,
            requestMemory,
            copyHttpProtocolErrorInfo(
                requestMemory.resource(),
                failure->protocolError()),
            baseRouteServices);
        failure->applyRequiredResponseHeaders(response);
        const auto connectionPlan = requireHttp1FinalResponseCommit(
            response, parsed.connectionPlan.requireClose());
        co_return Http1SessionRequestCompletion::makeBufferedClosing(
            connectionPlan);
    }
    const auto& webSocketEndpoint =
        *resolved.route().endpoint().webSocket();
    using Connection = SocketWebSocketConnection<Stream>;
    std::optional<Connection> webSocketConnection;
    auto upgradeAndRun = [&](Context& context) -> Task<void> {
        const auto handshake = makeHttpWebSocketServerHandshake(
            parsed.request,
            webSocketEndpoint.subprotocols());
        if (const auto ec = co_await writeWebSocketHandshake(stream, handshake); ec) {
            co_return;
        }
        webSocketConnection.emplace(
            WebSocketSocketTransport<Stream>{stream},
            scannerEntry,
            webSocketEndpoint.lifecycle(),
            ProtocolByteLimit::limited(options.maxWebSocketMessageBytes),
            memory.resource(),
            pendingFrames,
            handshake.negotiation().deflate());
        co_await invokeWebSocketHandler(
            *webSocketConnection,
            scannerEntry,
            webSocketEndpoint.handler(),
            context);
    };
    const auto terminal = makeCallableRef<void, Context&>(upgradeAndRun);
    std::optional<HttpResponse> buffered;
    std::exception_ptr exception;
    try {
        buffered = co_await routes.dispatchWebSocket(
            parsed.request,
            resolved,
            requestMemory,
            terminal,
            baseRouteServices);
    } catch (...) {
        exception = std::current_exception();
    }
    if (webSocketConnection.has_value()) {
        co_await finishWebSocketSession(*webSocketConnection, exception);
        co_return std::nullopt;
    }
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (buffered.has_value()) {
        response = std::move(*buffered);
        const auto connectionPlan = requireHttp1FinalResponseCommit(
            response, parsed.connectionPlan.requireClose());
        co_return Http1SessionRequestCompletion::makeBufferedClosing(
            connectionPlan);
    }
    co_return std::nullopt;
}

}  // namespace ruvia::detail
