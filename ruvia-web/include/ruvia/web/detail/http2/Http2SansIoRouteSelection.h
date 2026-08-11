#pragma once

#include <utility>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/http2/message/Http2RequestBuilder.h"
#include "ruvia/http/detail/http2/stream/Http2StreamState.h"
#include "ruvia/http/detail/http2/message/Http2WebSocketHandshake.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/router/RouteTable.h"

namespace ruvia::detail {

// Owner-side route policy for one HTTP/2 stream, run at kMessageHead so the
// body-mode and tunnel decisions land BEFORE the next feed. Returns the stream's
// runtime once a route is selected, or nullptr when the stream cannot be served.
[[nodiscard]] inline Http2SansIoStreamRuntime* http2SelectStreamRoute(const RouteTable& routes, Http2SansIoStreamRuntimeTable& streamRuntimes, Http2StreamState& streamState) {
    const auto method = Http2RequestBuilder::routeMethod(streamState);
    const auto path = Http2RequestBuilder::requestPath(streamState);
    auto& runtime = streamRuntimes.ensureAccepted(streamState);
    RouteResolution resolution;
    auto bodyMode = RequestBodyMode::kBuffered;
    if (!path.empty()) {
        // An unclassified method can only be served by an extension route, and
        // it is matched on the exact wire token -- the same split HTTP/1 makes
        // in RouteTable::resolve(const HttpRequest&). Without this branch,
        // extension routes would work over HTTP/1 and silently not over
        // HTTP/2.
        resolution = method == HttpKnownMethod::kUnknown ? routes.resolveExtensionMethod(streamState.requestMethod(), path) : routes.resolve(method, path);
    }
    const auto* resolved = resolution.resolved();
    if (resolved != nullptr) {
        bodyMode = resolved->route().endpoint().requestBodyMode();
    }
    if (http2IsPendingWebSocketConnect(streamState) && resolved != nullptr && resolved->route().endpoint().webSocket() != nullptr) {
        bodyMode = RequestBodyMode::kStream;
    }
    return runtime.selectRoute(std::move(resolution), bodyMode) ? &runtime : nullptr;
}

}  // namespace ruvia::detail
