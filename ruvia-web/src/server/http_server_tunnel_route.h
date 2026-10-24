#pragma once

#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <variant>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_response_server.h"

#include "http/http_socket_tunnel_transport.h"
#include "http/http_tunnel_session.h"
#include "http/tls_tunnel_output.h"
#include "server/http1_route_dispatch.h"
#include "server/http1_session_request_completion.h"
#include "server/http1_upgrade_response_head.h"
#include "server/http_request_admission.h"
#include "server/http_server_response_state.h"
#include "server/http_upgrade_route.h"
#include "server/inbound_transport_error.h"

namespace ruvia::detail {

// Admission already validated a CONNECT-UDP head (`udp`). HTTP/1 framing adds
// one rule of its own: a CONNECT request cannot carry request content. A
// successful handshake is access-logged through `tail` once its head reaches
// the transport; a buffered answer is left to the session's ordinary tail.
[[nodiscard]] inline http1_response_head_plan prepare_http1_tunnel_response_head_plan(
    const http_response& head_response, http_protocol_version version, bool udp) {
    if (!udp) {
        return prepare_http1_connect_response_head(head_response, version);
    }
    const auto upgrade = prepare_http1_connect_udp_response_head(head_response);
    if (upgrade.index() != 0) {
        throw http_protocol_error(http_status::internal_server_error,
            "invalid_connect_udp_response_head", "invalid CONNECT-UDP response head");
    }
    return std::get<0>(upgrade);
}

template <typename stream_type>
struct http1_tunnel_upgrade_commit final {
    using transport_type = http_socket_tunnel_transport<stream_type>;

    http1_route_dispatch<stream_type>& dispatch_;
    http_response_tail& tail_;
    bool udp_;
    std::optional<tls_tunnel_output>& tls_output_;

    [[nodiscard]] task<transport_type> operator()(
        context& context_value, const http_response& head_response) const {
        const auto plan = prepare_http1_tunnel_response_head_plan(
            head_response, dispatch_.parsed_.request_.protocol_version(), udp_);
        http_response_head_buffer head(
            std::pmr::polymorphic_allocator<char>(dispatch_.memory_.resource()));
        append_http1_response_head(head_response, head, plan);
        context_access::commit_upgrade(context_value);
        if (const auto ec = co_await write_http1_upgrade_response_head(dispatch_.stream_,
                dispatch_.parsed_.body_plan_.expectations(), head_response.status(), head.view());
            ec) {
            throw inbound_transport_error(ec, "CONNECT head write");
        }
        tail_.record_access(dispatch_.options_.access_log_, head_response.status());
        if constexpr (requires { dispatch_.stream_.next_layer(); }) {
            tls_output_.emplace(*dispatch_.stream_.native_handle(), dispatch_.stream_.lowest_layer(),
                dispatch_.base_route_services_.worker(), *dispatch_.memory_.resource());
            tls_output_->start();
        }
        co_return transport_type(dispatch_.stream_, tls_output_ ? &*tls_output_ : nullptr);
    }
};

template <typename stream_type>
[[nodiscard]] task<std::optional<http1_session_request_completion>> dispatch_http_tunnel_route(
    http1_route_dispatch<stream_type> d, http_response_tail& tail, const resolved_route& resolved, bool udp,
    std::string_view pending) {
    if (d.parsed_.body_plan_.requires_consumption()) {
        // A generated rejection does not inherit the negotiated coding promise.
        tail.disable_response_coding();
        d.response_ = co_await d.routes_.handle_error(d.parsed_.request_, d.request_memory_,
            http_error_info({.status_ = http_status::bad_request, .message_ = "CONNECT route does not accept HTTP request content"}), d.base_route_services_);
        co_return close_with_framework_response(d.response_, d.parsed_.connection_plan_);
    }
    std::optional<tls_tunnel_output> tls_output;
    // The TLS output pump must be stopped and joined on every exit, including
    // a handler failure that execute_tunnel_upgrade_session surfaces.
    std::optional<http_response> buffered;
    std::exception_ptr failure;
    try {
        buffered = co_await execute_tunnel_upgrade_session<http_socket_tunnel_transport<stream_type>>(
            d.routes_, d.parsed_.request_, resolved, d.request_memory_, d.base_route_services_,
            d.options_, d.scanner_entry_, d.inbound_buffer_pool_, udp, pending,
            http1_tunnel_upgrade_commit<stream_type>{
                .dispatch_ = d,
                .tail_ = tail,
                .udp_ = udp,
                .tls_output_ = tls_output,
            });
    } catch (...) {
        failure = std::current_exception();
    }
    if (tls_output) {
        if (failure) {
            tls_output->abort();
        }
        co_await tls_output->join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (buffered) {
        d.response_ = std::move(*buffered);
        co_return finalize_closing_route_response(d.response_, d.parsed_.connection_plan_);
    }
    co_return std::nullopt;
}

}  // namespace ruvia::detail
