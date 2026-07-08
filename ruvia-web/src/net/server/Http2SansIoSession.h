#pragma once

// Buffered HTTP/2 server session over the sans-I/O core (ruvia-web).
//
// Packages the proven dispatch glue: it drives an Http2Connection with the generic
// pumpSansIoConnection, accumulates each request's DATA into the stream, and runs the
// real framework dispatch pipeline (Http2RequestBuilder -> RouteTable::resolve ->
// dispatchBuffered) at end-of-stream, submitting the buffered response back through the
// core. This is the sans-I/O replacement for the coroutine Http2ServerSession's
// buffered path.
//
// SCOPE (additive, not yet wired into the accept loop): buffered requests + buffered
// responses only. Streaming responses, WebSocket tunnels, per-stream CONCURRENT
// dispatch, rate limiting, body-size limits and access logging are still handled only
// by the coroutine session and remain to reach full parity before this can replace it.

#include <string_view>

#include "HttpRequestInternal.h"
#include "HttpResponseBodyAccess.h"
#include "http/ContextServices.h"
#include "net/http2/Http2Connection.h"
#include "net/http2/Http2RequestBuilder.h"
#include "router/RouteResolution.h"
#include "router/RouteTable.h"
#include "runtime/SansIoDriver.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

template <typename Stream>
Task<void> runHttp2SansIoBufferedSession(
    Stream& stream, RouteTable& routes, WorkerMemory& worker, std::string_view remoteAddress) {
    Http2Connection connection(worker.resource());
    connection.expectClientPreface();
    connection.queueLocalSettings();

    auto onReadable = [&worker, &routes, remoteAddress](Http2Connection& conn) -> Task<void> {
        for (;;) {
            const auto event = conn.nextEvent();
            if (event.kind == Http2Event::Kind::kNone) {
                break;
            }
            auto* streamState = conn.stream(event.streamId);
            if (streamState == nullptr) {
                continue;
            }
            if (event.kind == Http2Event::Kind::kRequestBodyChunk) {
                // The core does not buffer request bodies (that is owner policy); the
                // buffered path re-accumulates them into the stream so the builder can
                // hand the whole body to the handler. Size limits were already enforced
                // by the core's DATA accounting.
                streamState->appendRequestBody(event.bytes);
                continue;
            }
            if (event.kind != Http2Event::Kind::kRequestEnd) {
                continue;
            }

            RequestMemory requestMemory(worker);
            HttpRequest request = HttpRequestAccess::make();
            if (!Http2RequestBuilder::build(*streamState, request, requestMemory.resource())) {
                conn.submitReset(
                    event.streamId, static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
                continue;
            }
            HttpRequestAccess::setTransport(request, remoteAddress, std::string_view{}, false);

            RouteMatch match;
            const auto resolution = routes.resolve(request, match);
            HttpResponse response = co_await routes.dispatchBuffered(
                request, resolution, requestMemory, /*closeConnectionOnError=*/false,
                ContextServices{});
            if (streamState->isReset()) {
                continue;
            }
            const bool bodyForbidden =
                Http2RequestBuilder::requestMethod(*streamState) == HttpMethod::kHead;
            conn.submitResponseHead(event.streamId, response, bodyForbidden);
            conn.submitData(
                event.streamId, responseBodyBytes(response), /*endStream=*/true);
        }
        co_return;
    };

    co_await pumpSansIoConnection(connection, stream, onReadable);
}

}  // namespace ruvia::detail
