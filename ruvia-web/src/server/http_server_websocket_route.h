#pragma once

#include <exception>
#include <optional>
#include <string_view>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/websocket_handshake.h"

#include "http/http_protocol_error_info.h"
#include "router/route_table.h"
#include "server/http1_route_dispatch.h"
#include "server/http1_session_request_completion.h"
#include "server/http_server_options.h"
#include "server/http_server_response_state.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/http_websocket_handshake.h"
#include "websocket/http_websocket_session.h"
#include "websocket/http_websocket_socket_transport.h"
#include "websocket/websocket_response_headers.h"

namespace ruvia::detail {

// A rejected upgrade returns the exact HTTP/1 request completion that the
// session must write and clean up. A successful upgrade transfers transport
// ownership to the websocket session, so no HTTP request completion remains.
template <typename stream_type>
task<std::optional<http1_session_request_completion>> dispatch_http_websocket_route(
    http1_route_dispatch<stream_type> d, const resolved_route& resolved, std::string_view pending_frames) {
    const auto handshake_validation =
        validate_websocket_handshake(d.parsed_.request_, d.parsed_.body_plan_);
    if (const auto* failure = handshake_validation.failure()) {
        d.response_ = co_await d.routes_.handle_error(d.parsed_.request_, d.request_memory_,
            copy_http_protocol_error_info(d.request_memory_.resource(), failure->protocol_error()),
            d.base_route_services_);
        failure->apply_required_response_headers(d.response_);
        const auto connection_plan =
            require_http1_final_response_commit(d.response_, d.parsed_.connection_plan_.require_close());
        co_return http1_session_request_completion::make_buffered_closing(connection_plan);
    }
    const auto& websocket_endpoint = *resolved.route().endpoint().get_websocket();
    using connection_type = socket_websocket_connection_type<stream_type>;
    std::optional<connection_type> websocket_connection;
    auto upgrade_and_run = [&](context& context_value) -> task<void> {
        const auto response_headers_value = websocket_response_headers(context_value);
        const auto handshake = make_websocket_server_handshake(
            d.parsed_.request_, {.supported_subprotocols_ = websocket_endpoint.subprotocols(),
                                    .response_headers_ = response_headers_value,
                                    .resource_ = d.memory_.resource(),
                                    .deflate_ = websocket_endpoint.deflate()});
        context_access::mark_websocket_handshake_started(context_value);
        if (const auto ec = co_await write_websocket_handshake(d.stream_, handshake); ec) {
            co_return;
        }
        websocket_connection.emplace(websocket_socket_transport<stream_type>{d.stream_},
            d.base_route_services_.worker(), d.scanner_entry_, websocket_endpoint.lifecycle(),
            protocol_byte_limit::limited(d.options_.max_websocket_message_bytes_),
            d.inbound_buffer_pool_ != nullptr ? d.inbound_buffer_pool_ : d.memory_.resource(),
            pending_frames, handshake.compression(), websocket_endpoint.deflate().compression_level_);
        co_await invoke_websocket_handler(
            *websocket_connection, d.scanner_entry_, websocket_endpoint.handler(), context_value);
    };
    const auto terminal = make_callable_ref<void, context&>(upgrade_and_run);
    std::optional<http_response> buffered;
    std::exception_ptr exception;
    try {
        buffered = co_await d.routes_.dispatch_websocket(
            d.parsed_.request_, resolved, d.request_memory_, terminal, d.base_route_services_);
    } catch (...) {
        exception = std::current_exception();
    }
    if (websocket_connection.has_value()) {
        co_await finish_websocket_session(*websocket_connection, exception,
            d.options_.connection_failure_, d.base_route_services_.get_conn_info().remote().address());
        co_return std::nullopt;
    }
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (buffered.has_value()) {
        d.response_ = std::move(*buffered);
        const auto connection_plan =
            require_http1_final_response_commit(d.response_, d.parsed_.connection_plan_.require_close());
        co_return http1_session_request_completion::make_buffered_closing(connection_plan);
    }
    co_return std::nullopt;
}

}  // namespace ruvia::detail
