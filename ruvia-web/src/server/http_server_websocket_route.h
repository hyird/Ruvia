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
struct http1_websocket_upgrade_operation final {
    using connection_type = websocket_connection<stream_type>;

    http1_route_dispatch<stream_type>& dispatch_;
    http_response_tail& tail_;
    const resolved_route& resolved_;
    const websocket_route_endpoint& websocket_endpoint_;
    std::string_view pending_frames_;
    std::optional<connection_type>& websocket_connection_;

    [[nodiscard]] task<void> operator()(context& context_value) {
        const auto response_headers_value = websocket_response_headers(context_value);
        const auto handshake = make_websocket_server_handshake(
            dispatch_.parsed_.request_, {.supported_subprotocols_ = websocket_endpoint_.subprotocols(),
                                            .response_headers_ = response_headers_value,
                                            .resource_ = dispatch_.memory_.resource(),
                                            .deflate_ = websocket_endpoint_.deflate()});
        context_access::commit_upgrade(context_value);
        if (const auto ec = co_await write_websocket_handshake(dispatch_.stream_, handshake); ec) {
            co_return;
        }
        tail_.record_access(dispatch_.options_.access_log_, http_status::switching_protocols);
        const auto& deflate_config = websocket_endpoint_.deflate();
        // Directions without context takeover borrow the worker's codecs, which
        // outlive every connection, so they allocate no codec state here.
        websocket_connection_.emplace(dispatch_.stream_, dispatch_.base_route_services_.worker(),
            dispatch_.scanner_entry_, websocket_endpoint_.lifecycle(),
            protocol_byte_limit::limited(dispatch_.options_.server_.max_websocket_message_bytes_),
            &dispatch_.inbound_buffer_pool_, pending_frames_,
            websocket_server_protocol_options{.compression_ = handshake.compression(),
                .compression_level_ = deflate_config.compression_level_,
                .mem_level_ = deflate_config.mem_level_,
                .shared_deflate_ = &dispatch_.options_.codecs_->websocket_deflate()});
        co_await invoke_websocket_handler(
            *websocket_connection_, dispatch_.scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(connection_type& connection_value, std::exception_ptr failure) const {
        return finish_websocket_session(connection_value, std::move(failure),
            dispatch_.options_.connection_failure_,
            dispatch_.base_route_services_.get_conn_info().remote().address());
    }
};

template <typename stream_type>
task<std::optional<http1_session_request_completion>> dispatch_http_websocket_route(
    http1_route_dispatch<stream_type> d, http_response_tail& tail, const resolved_route& resolved,
    std::string_view pending_frames) {
    const auto& websocket_endpoint = *resolved.route().endpoint().get_websocket();
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
    using upgrade_type = http1_websocket_upgrade_operation<stream_type>;
    std::optional<typename upgrade_type::connection_type> websocket_connection;
    upgrade_type upgrade{
        .dispatch_ = d,
        .tail_ = tail,
        .resolved_ = resolved,
        .websocket_endpoint_ = websocket_endpoint,
        .pending_frames_ = pending_frames,
        .websocket_connection_ = websocket_connection,
    };
    auto outcome = co_await run_http_upgrade_route(d.routes_, d.parsed_.request_,
        resolved, d.request_memory_, d.base_route_services_, websocket_connection, upgrade, upgrade);
    if (auto* buffered = outcome.response()) {
        d.response_ = std::move(*buffered);
        co_return finalize_closing_route_response(d.response_, d.parsed_.connection_plan_);
    }
    co_return std::nullopt;
}

}  // namespace ruvia::detail
