#pragma once

#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <variant>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_connect.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_response_server.h"

#include "http/http_socket_tunnel_transport.h"
#include "http/http_tunnel_session.h"
#include "http/tls_tunnel_output.h"
#include "server/http1_route_dispatch.h"
#include "server/http1_session_request_completion.h"
#include "server/http_request_admission.h"
#include "server/http_server_response_state.h"
#include "server/http_upgrade_route.h"
#include "server/inbound_transport_error.h"

namespace ruvia::detail {

// Admission already validated a CONNECT-UDP head (`udp`). HTTP/1 framing adds
// one rule of its own: a CONNECT request cannot carry request content. A
// successful handshake is access-logged through `tail` once its head reaches
// the transport; a buffered answer is left to the session's ordinary tail.
[[nodiscard]] inline std::variant<http1_response_head_plan, http_protocol_error>
prepare_http1_tunnel_response_head_plan(
    const http_response& head_response, http_protocol_version version, bool udp) {
    if (!udp) {
        return prepare_http1_connect_response_head(head_response, version);
    }
    const auto upgrade = prepare_http1_connect_udp_response_head(head_response);
    if ((upgrade.index() != 0)) {
        return http_protocol_error(http_status::internal_server_error, "invalid CONNECT-UDP response head");
    }
    return std::get<0>(upgrade);
}

template <typename stream_type>
struct http1_tunnel_upgrade_operation final {
    using transport_type = http_socket_tunnel_transport<stream_type>;
    using session_type = http_tunnel_session<transport_type>;

    http1_route_dispatch<stream_type>& dispatch_;
    http_response_tail& tail_;
    const resolved_route& resolved_;
    const tunnel_route_endpoint& endpoint_;
    bool udp_;
    std::string_view pending_;
    std::optional<tls_tunnel_output>& tls_output_;
    std::optional<session_type>& tunnel_;

    [[nodiscard]] task<void> operator()(context& context_value) {
        auto head_response = context_access::streaming_head(context_value);
        if (udp_) {
            auto negotiated = prepare_http_connect_udp_response(
                std::move(head_response), dispatch_.parsed_.request_.protocol_version());
            if ((negotiated.index() != 0)) {
                throw std::invalid_argument("invalid CONNECT-UDP response metadata");
            }
            head_response = std::move(std::get<0>(negotiated));
        }
        const auto plan = prepare_http1_tunnel_response_head_plan(
            head_response, dispatch_.parsed_.request_.protocol_version(), udp_);
        if (plan.index() != 0) {
            throw std::get<1>(plan);
        }
        http_response_head_buffer head(
            std::pmr::polymorphic_allocator<char>(dispatch_.memory_.resource()));
        append_http1_response_head(head_response, head, std::get<0>(plan));
        context_access::commit_upgrade(context_value);
        const auto written = co_await async_asio<std::size_t>([this, &head](auto handler) {
            asio::async_write(dispatch_.stream_, asio::buffer(head.view()), std::move(handler));
        });
        if (written.error_code()) {
            throw inbound_transport_error(written.error_code(), "CONNECT head write");
        }
        tail_.record_access(dispatch_.options_.access_log_, head_response.status());
        if constexpr (requires { dispatch_.stream_.next_layer(); }) {
            tls_output_.emplace(*dispatch_.stream_.native_handle(), dispatch_.stream_.next_layer(),
                dispatch_.base_route_services_.worker(), *dispatch_.memory_.resource());
            tls_output_->start();
        }
        tunnel_.emplace(transport_type(dispatch_.stream_, tls_output_ ? &*tls_output_ : nullptr),
            dispatch_.base_route_services_.worker(), *dispatch_.memory_.resource(),
            dispatch_.inbound_buffer_pool_, pending_);
        co_await invoke_tunnel_handler(
            *tunnel_, dispatch_.scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_tunnel_session(session_value, std::move(failure),
            dispatch_.options_.connection_failure_,
            dispatch_.base_route_services_.get_conn_info().remote().address(),
            dispatch_.scanner_entry_, endpoint_.config().peer_transport_fin_timeout_);
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
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    using upgrade_type = http1_tunnel_upgrade_operation<stream_type>;
    std::optional<tls_tunnel_output> tls_output;
    std::optional<typename upgrade_type::session_type> tunnel;
    upgrade_type upgrade{
        .dispatch_ = d,
        .tail_ = tail,
        .resolved_ = resolved,
        .endpoint_ = endpoint,
        .udp_ = udp,
        .pending_ = pending,
        .tls_output_ = tls_output,
        .tunnel_ = tunnel,
    };
    // The TLS output pump must be stopped and joined on every exit, including
    // a handler failure that run_http_upgrade_route surfaces.
    std::optional<http_upgrade_route_outcome> outcome;
    std::exception_ptr failure;
    try {
        outcome.emplace(co_await run_http_upgrade_route(d.routes_, d.parsed_.request_,
            resolved, d.request_memory_, d.base_route_services_, tunnel, upgrade, upgrade));
    } catch (...) {
        failure = std::current_exception();
    }
    if (tls_output) {
        if (!tunnel) {
            tls_output->abort();
        }
        co_await tls_output->join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (auto* buffered = outcome->response()) {
        d.response_ = std::move(*buffered);
        co_return finalize_closing_route_response(d.response_, d.parsed_.connection_plan_);
    }
    co_return std::nullopt;
}

}  // namespace ruvia::detail
