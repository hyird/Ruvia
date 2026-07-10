#pragma once

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
#include "ruvia/web/detail/websocket/HttpWebSocketHandshake.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/detail/HttpParserInternal.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/core/memory/MemoryPool.h"

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
    ContextServices baseRouteServices,
    const HttpServerOptions& options,
    std::string_view pendingFrames,
    HttpResponse& response,
    bool& closeAfterWrite) {
    if (!isValidWebSocketRequest(parsed.request, parsed.flags) || parsed.contentLength != 0 || parsed.chunked) {
        response = co_await routes.handleError(
            parsed.request,
            requestMemory,
            HttpErrorInfo(400, {}, "invalid websocket upgrade"),
            baseRouteServices);
        markConnectionCloseAfterWrite(response, closeAfterWrite);
        co_return HttpWebSocketRouteResult::kWriteBufferedResponse;
    }
    bool permessageDeflate = false;
    const auto& route = routeResolution.route();
    if (!(co_await writeWebSocketHandshake(
            stream,
            parsed.request,
            parsed.flags,
            route.webSocketSubprotocols(),
            permessageDeflate))) {
        co_return HttpWebSocketRouteResult::kSessionFinished;
    }

    SocketWebSocketConnection<Stream> webSocketConnection(
        WebSocketSocketTransport<Stream>{stream},
        scannerEntry,
        route.webSocketHeartbeat(),
        options.maxWebSocketMessageBytes,
        memory.resource(),
        pendingFrames,
        permessageDeflate);
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
