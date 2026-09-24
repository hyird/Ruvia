#pragma once

#include <utility>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/router/RouteTable.h"

namespace ruvia::detail {

// Owner-side route policy for one HTTP/2 stream, run at kMessageHead so the
// body-mode and tunnel decisions land BEFORE the next feed. Returns the stream's
// runtime once a route is selected, or nullptr when the stream cannot be served.
[[nodiscard]] inline Http2SansIoStreamRuntime* http2SelectStreamRoute(const RouteTable& routes,
    const Http2ServerRequestRouteView& route, Http2SansIoStreamRuntimeTable& streamRuntimes,
    std::uint32_t streamId) {
    const auto method = route.method;
    const auto path = route.path;
    auto& runtime = streamRuntimes.ensureAccepted(streamId);
    RouteResolution resolution;
    auto bodyMode = RequestBodyMode::kBuffered;
    if (!path.empty()) {
        // An unclassified method can only be served by an extension route, and
        // it is matched on the exact wire token -- the same split HTTP/1 makes
        // in RouteTable::resolve(const HttpRequest&). Without this branch,
        // extension routes would work over HTTP/1 and silently not over
        // HTTP/2.
        resolution = method == HttpKnownMethod::kUnknown
                         ? routes.resolveExtensionMethod(route.requestMethod, path)
                         : routes.resolve(method, path);
    }
    const auto* resolved = resolution.resolved();
    if (resolved != nullptr) {
        bodyMode = resolved->route().endpoint().requestBodyMode();
    }
    if (route.webSocketConnect && resolved != nullptr &&
        resolved->route().endpoint().webSocket() != nullptr) {
        bodyMode = RequestBodyMode::kStream;
    }
    return runtime.selectRoute(std::move(resolution), bodyMode) ? &runtime : nullptr;
}

}  // namespace ruvia::detail
