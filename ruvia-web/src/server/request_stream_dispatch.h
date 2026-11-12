#pragma once

#include "ruvia/core/task.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"
#include "ruvia/web/request_stream.h"

#include "router/route_resolution.h"

// listener_dispatch::request_stream: what a request stream connection resolves
// every request to in place of the route table, and the one run of the
// application's request_stream callback every transport reaches through it.

namespace ruvia::detail {

class route_entry;

// A request stream route runs no middleware and declares no body limit. A
// tunnel request -- CONNECT, extended CONNECT, or an HTTP/1.1 Upgrade without
// content -- resolves to an upgrade route whose callback commits the tunnel
// itself (accept_tunnel()); any other request to a response stream route with
// a streamed body.
[[nodiscard]] route_resolution resolve_request_stream_route(bool tunnel) noexcept;

// Whether `route` is the tunnel route resolve_request_stream_route() yields.
[[nodiscard]] bool is_request_stream_tunnel_route(const route_entry& route) noexcept;

// Whether an HTTP/1 request of a request stream connection is a tunnel
// request; `content` is whether it carries request content.
[[nodiscard]] bool http1_request_stream_tunnel(const http_request& request, bool content) noexcept;

// Runs the connection's callback for a tunnel request in the context its
// upgrade route built. `accept` commits the success head and establishes the
// tunnel; a response the callback writes instead becomes the context's
// buffered response.
[[nodiscard]] task<void> serve_request_stream_tunnel(context& context_value, const request_stream_tunnel_accept& accept);

}  // namespace ruvia::detail
