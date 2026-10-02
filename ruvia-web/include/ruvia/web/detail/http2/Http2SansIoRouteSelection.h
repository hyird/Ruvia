#pragma once

#include <utility>

#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/router/RouteTable.h"

namespace ruvia::detail {

// Owner-side route policy for one HTTP/2 stream, run at kMessageHead so the
// body-mode and tunnel decisions land BEFORE the next feed. Returns the stream's
// runtime once a route is selected, or nullptr when the stream cannot be served.
[[nodiscard]] inline Http2SansIoStreamRuntime* http2SelectStreamRoute(const RouteTable& routes,
    const http2_server_request_view& request, Http2SansIoStreamRuntimeTable& streamRuntimes,
    std::uint32_t streamId) {
    const auto method = classifyHttpMethod(request.method);
    const auto query = request.path.find('?');
    const auto path = request.path.substr(0, query);
    const bool webSocketConnect = method == HttpKnownMethod::kConnect &&
                                  httpAsciiEqualsIgnoreCase(request.protocol, "websocket");
    auto& runtime = streamRuntimes.ensureAccepted(streamId);
    RouteResolution resolution;
    auto bodyMode = RequestBodyMode::kBuffered;
    if (method == HttpKnownMethod::kConnect && !webSocketConnect) {
        resolution = routes.resolveConnect(request.protocol, request.protocol.empty() ? request.authority : path);
    } else if (!path.empty()) {
        // An unclassified method can only be served by an extension route, and
        // it is matched on the exact wire token -- the same split HTTP/1 makes
        // in RouteTable::resolve(const HttpRequest&). Without this branch,
        // extension routes would work over HTTP/1 and silently not over
        // HTTP/2.
        resolution = method == HttpKnownMethod::kUnknown
                         ? routes.resolveExtensionMethod(request.method, path)
                         : routes.resolve(webSocketConnect ? HttpKnownMethod::kGet : method, path);
    }
    const auto* resolved = resolution.resolved();
    if (resolved != nullptr) {
        bodyMode = resolved->route().endpoint().requestBodyMode();
    }
    if (resolved != nullptr && ((webSocketConnect && resolved->route().endpoint().webSocket() != nullptr) ||
                                   resolved->route().endpoint().tunnel() != nullptr)) {
        bodyMode = RequestBodyMode::kStream;
    }
    return runtime.selectRoute(std::move(resolution), bodyMode) ? &runtime : nullptr;
}

}  // namespace ruvia::detail
