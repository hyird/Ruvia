#pragma once

#include "ConnectionScanner.h"
#include "HttpServerResponseState.h"
#include "../ws/HttpWebSocketConnection.h"
#include "../ws/HttpWebSocketSocketTransport.h"
#include "../ws/HttpWebSocketHandshake.h"
#include "../ws/HttpWebSocketUtils.h"
#include "../../http/HttpParserInternal.h"
#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <exception>
#include <string_view>

namespace ruvia::detail {

enum class HttpWebSocketRouteResult {
    kWriteBufferedResponse,
    kSessionFinished
};

template <typename Stream>
Task<HttpWebSocketRouteResult> dispatchHttpWebSocketRoute(
    Stream& stream,
    WorkerMemory& memory,
    ConnectionScanner::Entry& scannerEntry,
    const HttpServerParseResult& parsed,
    const RouteResolution& routeResolution,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    RouteServices baseRouteServices,
    const HttpServerOptions& options,
    std::string_view pendingFrames,
    HttpResponse& response,
    bool& closeAfterWrite) {
    if (!isValidWebSocketRequest(parsed.request, parsed.flags) || parsed.contentLength != 0 || parsed.chunked) {
        response = co_await routes.handleError(
            parsed.request,
            requestMemory,
            HttpErrorInfo{.statusCode = 400, .message = "invalid websocket upgrade"},
            true,
            baseRouteServices);
        markConnectionCloseAfterWrite(response, closeAfterWrite);
        co_return HttpWebSocketRouteResult::kWriteBufferedResponse;
    }
    if (!(co_await writeWebSocketHandshake(
            stream,
            parsed.request,
            parsed.flags,
            routeResolution.route->webSocketSubprotocols))) {
        co_return HttpWebSocketRouteResult::kSessionFinished;
    }

    SocketWebSocketConnection<Stream> webSocketConnection(
        WebSocketSocketTransport<Stream>{stream},
        scannerEntry,
        routeResolution.route->webSocketHeartbeat,
        options.maxWebSocketMessageBytes,
        memory.resource(),
        pendingFrames);
    WebSocket webSocket(
        &webSocketConnection,
        &SocketWebSocketConnection<Stream>::readThunk,
        &SocketWebSocketConnection<Stream>::writeThunk,
        &SocketWebSocketConnection<Stream>::closeThunk);

    std::exception_ptr webSocketException;
    try {
        scannerEntry.setPhase(ConnectionScanner::Phase::kWebSocket);
        (void)co_await routes.dispatchWebSocket(
            parsed.request,
            routeResolution,
            requestMemory,
            webSocket,
            baseRouteServices);
    } catch (...) {
        webSocketException = std::current_exception();
    }
    if (webSocketException != nullptr) {
        try {
            co_await webSocketConnection.close(1011, "internal server error");
        } catch (...) {
        }
    }
    co_await webSocketConnection.detachAndDrainBackgroundWrites();
    co_return HttpWebSocketRouteResult::kSessionFinished;
}

}  // namespace ruvia::detail
