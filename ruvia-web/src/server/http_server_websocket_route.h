#pragma once

#include <exception>
#include <optional>
#include <string_view>

#include "ruvia/core/task.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/websocket_handshake.h"

#include "http/http_protocol_error_info.h"
#include "router/route_table.h"
#include "server/http1_route_dispatch.h"
#include "server/http1_session_request_completion.h"
#include "server/http_request_admission.h"
#include "server/http_server_options.h"
#include "server/http_server_response_state.h"
#include "server/http_upgrade_route.h"
#include "server/worker_codecs.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/http_websocket_handshake.h"
#include "websocket/http_websocket_session.h"
#include "websocket/websocket_response_headers.h"

namespace ruvia::detail {

// A rejected upgrade returns the exact HTTP/1 request completion that the
// session must write and clean up. A successful upgrade is access-logged
// through `tail` once its handshake reaches the transport, then transfers
// transport ownership to the websocket session, so no HTTP request completion
// remains.
template <typename stream_type>
struct http1_websocket_upgrade_commit final {
    http1_route_dispatch<stream_type>& dispatch_;
    http_response_tail& tail_;
    const websocket_route_endpoint& websocket_endpoint_;

    [[nodiscard]] task<std::optional<websocket_upgrade_commit<stream_type>>> operator()(
        context& context_value, std::span<const http_header_view> response_headers_value) const {
        const auto handshake = make_websocket_server_handshake(
            dispatch_.parsed_.request_, {.supported_subprotocols_ = websocket_endpoint_.subprotocols(),
                                            .response_headers_ = response_headers_value,
                                            .resource_ = dispatch_.memory_.resource(),
                                            .deflate_ = websocket_endpoint_.deflate()});
        context_access::commit_upgrade(context_value);
        if (const auto ec = co_await write_websocket_handshake(
                dispatch_.stream_, handshake, dispatch_.parsed_.body_plan_.expectations());
            ec) {
            co_return std::nullopt;
        }
        tail_.record_access(dispatch_.options_.access_log_, http_status::switching_protocols);
        co_return websocket_upgrade_commit<stream_type>(dispatch_.stream_, handshake.compression());
    }
};

template <typename stream_type>
task<std::optional<http1_session_request_completion>> dispatch_http_websocket_route(
    http1_route_dispatch<stream_type> d, http_response_tail& tail, const resolved_route& resolved,
    std::string_view pending_frames) {
    const auto& websocket_endpoint = *resolved.route().endpoint().websocket();
    const auto handshake_validation = validate_websocket_handshake(
        d.parsed_.request_, d.parsed_.body_plan_, websocket_endpoint.handshake_validation());
    if (const auto* failure = handshake_validation.failure()) {
        // A generated rejection does not inherit the negotiated coding promise.
        tail.disable_response_coding();
        d.response_ = co_await d.routes_.handle_error(d.parsed_.request_, d.request_memory_,
            copy_http_protocol_error_info(d.request_memory_.resource(), failure->protocol_error()),
            d.base_route_services_);
        failure->apply_required_response_headers(d.response_);
        co_return close_with_framework_response(d.response_, d.parsed_.connection_plan_);
    }
    auto buffered = co_await execute_websocket_upgrade_session<stream_type>(
        d.routes_, d.parsed_.request_, resolved, d.request_memory_, d.base_route_services_,
        d.options_, d.scanner_entry_, d.inbound_buffer_pool_, pending_frames,
        http1_websocket_upgrade_commit<stream_type>{
            .dispatch_ = d,
            .tail_ = tail,
            .websocket_endpoint_ = websocket_endpoint,
        });
    if (buffered) {
        d.response_ = std::move(*buffered);
        co_return finalize_closing_route_response(d.response_, d.parsed_.connection_plan_);
    }
    co_return std::nullopt;
}

}  // namespace ruvia::detail
