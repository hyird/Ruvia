#pragma once

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/web/detail/server/route/Http1RouteDispatch.h"
#include "ruvia/web/detail/server/http1/Http1SessionRequestCompletion.h"
#include "ruvia/web/detail/server/response/HttpServerResponseState.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
#include "ruvia/web/detail/websocket/HttpWebSocketHandshake.h"
#include "ruvia/web/detail/http/error/HttpProtocolErrorInfo.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeValidation.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/Task.h"
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
    Http1RouteDispatch<Stream> d,
    const ResolvedRoute& resolved,
    std::string_view pendingFrames) {
    const auto handshakeValidation = validateHttp1WebSocketHandshake(
        d.parsed.request,
        d.parsed.bodyPlan);
    if (const auto* failure = handshakeValidation.failure()) {
        d.response = co_await d.routes.handleError(
            d.parsed.request,
            d.requestMemory,
            copyHttpProtocolErrorInfo(
                d.requestMemory.resource(),
                failure->protocolError()),
            d.baseRouteServices);
        failure->applyRequiredResponseHeaders(d.response);
        const auto connectionPlan = requireHttp1FinalResponseCommit(
            d.response, d.parsed.connectionPlan.requireClose());
        co_return Http1SessionRequestCompletion::makeBufferedClosing(
            connectionPlan);
    }
    const auto& webSocketEndpoint =
        *resolved.route().endpoint().webSocket();
    using Connection = SocketWebSocketConnection<Stream>;
    std::optional<Connection> webSocketConnection;
    auto upgradeAndRun = [&](Context& context) -> Task<void> {
        const auto handshake = makeHttpWebSocketServerHandshake(
            d.parsed.request,
            webSocketEndpoint.subprotocols(),
            d.memory.resource());
        if (const auto ec = co_await writeWebSocketHandshake(d.stream, handshake); ec) {
            co_return;
        }
        webSocketConnection.emplace(
            WebSocketSocketTransport<Stream>{d.stream},
            d.baseRouteServices.worker(),
            d.scannerEntry,
            webSocketEndpoint.lifecycle(),
            ProtocolByteLimit::limited(d.options.maxWebSocketMessageBytes),
            d.memory.resource(),
            pendingFrames,
            handshake.negotiation().deflate());
        co_await invokeWebSocketHandler(
            *webSocketConnection,
            d.scannerEntry,
            webSocketEndpoint.handler(),
            context);
    };
    const auto terminal = makeCallableRef<void, Context&>(upgradeAndRun);
    std::optional<HttpResponse> buffered;
    std::exception_ptr exception;
    try {
        buffered = co_await d.routes.dispatchWebSocket(
            d.parsed.request,
            resolved,
            d.requestMemory,
            terminal,
            d.baseRouteServices);
    } catch (...) {
        exception = std::current_exception();
    }
    if (webSocketConnection.has_value()) {
        co_await finishWebSocketSession(
            *webSocketConnection,
            exception,
            d.options.connectionFailure,
            d.baseRouteServices.connInfo().remote().address());
        co_return std::nullopt;
    }
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (buffered.has_value()) {
        d.response = std::move(*buffered);
        const auto connectionPlan = requireHttp1FinalResponseCommit(
            d.response, d.parsed.connectionPlan.requireClose());
        co_return Http1SessionRequestCompletion::makeBufferedClosing(
            connectionPlan);
    }
    co_return std::nullopt;
}

}  // namespace ruvia::detail
