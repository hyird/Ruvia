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
    // Preserve HTTP/2's accepted token normalization without widening HTTP/3's
    // exact-token boundary or materializing a request.
    const auto extended_protocol = httpAsciiEqualsIgnoreCase(request.protocol, "websocket")
                                       ? std::string_view("websocket")
                                       : request.protocol;
    auto& runtime = streamRuntimes.ensureAccepted(streamId);
    auto resolution = routes.resolve(route_request_view{
        method, request.method, path, request.authority, extended_protocol});
    auto bodyMode = RequestBodyMode::kBuffered;
    const auto* resolved = resolution.resolved();
    if (resolved != nullptr) {
        bodyMode = resolved->route().endpoint().requestBodyMode();
    }
    if (resolved != nullptr && ((method == HttpKnownMethod::kConnect && resolved->route().endpoint().webSocket() != nullptr) ||
                                   resolved->route().endpoint().tunnel() != nullptr)) {
        bodyMode = RequestBodyMode::kStream;
    }
    return runtime.selectRoute(std::move(resolution), bodyMode) ? &runtime : nullptr;
}

}  // namespace ruvia::detail
