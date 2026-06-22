#pragma once

#include "ConnectionScanner.h"
#include "HttpServerResponseState.h"
#include "../ws/HttpWebSocketConnection.h"
#include "../ws/HttpWebSocketSession.h"
#include "../ws/HttpWebSocketSocketTransport.h"
#include "../ws/HttpWebSocketHandshake.h"
#include "../ws/HttpWebSocketUtils.h"
#include "../../http/HttpParserInternal.h"
#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

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
    co_await runWebSocketSession(
        webSocketConnection,
        scannerEntry,
        routes,
        parsed.request,
        routeResolution,
        requestMemory,
        baseRouteServices);
    co_return HttpWebSocketRouteResult::kSessionFinished;
}

}  // namespace ruvia::detail
