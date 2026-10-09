#pragma once

#include <utility>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_known_method.h"

#include "http2/http2_sans_io_stream_runtime.h"
#include "router/route_table.h"

namespace ruvia::detail {

// Owner-side route policy for one HTTP/2 stream, run at message_head so the
// body-mode and tunnel decisions land BEFORE the next feed. Returns the stream's
// runtime once a route is selected, or nullptr when the stream cannot be served.
[[nodiscard]] inline http2_sans_io_stream_runtime* http2_select_stream_route(const route_table& routes_value,
    const http2_server_request_view& request, http2_sans_io_stream_runtime_table& stream_runtimes,
    std::uint32_t stream_id) {
    const auto method = classify_http_method(request.method_);
    const auto query = request.path_.find('?');
    const auto path = request.path_.substr(0, query);
    // Preserve HTTP/2's accepted token normalization without widening HTTP/3's
    // exact-token boundary or materializing a request.
    const auto extended_protocol = http_ascii_equals_ignore_case(request.protocol_, "websocket")
                                       ? std::string_view("websocket")
                                       : request.protocol_;
    auto& runtime = stream_runtimes.ensure_accepted(stream_id);
    auto resolution = routes_value.resolve(route_request_view{
        method, request.method_, path, request.authority_, extended_protocol});
    auto body_mode = request_body_mode::buffered;
    const auto* resolved = resolution.resolved();
    if (resolved != nullptr) {
        body_mode = resolved->route().endpoint().request_body_mode();
    }
    if (resolved != nullptr && ((method == http_known_method::connect && resolved->route().endpoint().get_websocket() != nullptr) ||
                                   resolved->route().endpoint().tunnel() != nullptr)) {
        body_mode = request_body_mode::stream;
    }
    return runtime.select_route(std::move(resolution), body_mode) ? &runtime : nullptr;
}

}  // namespace ruvia::detail
