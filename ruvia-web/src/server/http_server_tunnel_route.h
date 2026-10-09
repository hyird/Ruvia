#pragma once

#include <exception>
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
#include "ruvia/web/detail/util/callable_ref.h"

#include "http/http_socket_tunnel_transport.h"
#include "http/http_tunnel_session.h"
#include "http/tls_tunnel_output.h"
#include "server/http1_route_dispatch.h"
#include "server/http1_session_request_completion.h"
#include "server/http_server_response_state.h"

namespace ruvia::detail {

template <typename stream_type>
[[nodiscard]] task<std::optional<http1_session_request_completion>> dispatch_http_tunnel_route(
    http1_route_dispatch<stream_type> d, const resolved_route& resolved, std::string_view pending) {
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    const bool udp = endpoint.protocol() == "connect-udp";
    if (d.parsed_.body_plan_.requires_consumption() || (udp && (validate_http_connect_udp_request(d.parsed_.request_).index() != 0))) {
        d.response_ = co_await d.routes_.handle_error(d.parsed_.request_, d.request_memory_,
            http_error_info({.status_ = http_status::bad_request, .message_ = "CONNECT route does not accept HTTP request content"}), d.base_route_services_);
        co_return http1_session_request_completion::make_buffered_closing(
            require_http1_final_response_commit(d.response_, d.parsed_.connection_plan_.require_close()));
    }
    std::optional<tls_tunnel_output> tls_output;
    std::optional<http_tunnel_session<http_socket_tunnel_transport<stream_type>>> tunnel;
    auto establish_and_run = [&](context& context_value) -> task<void> {
        auto head_response = context_access::streaming_head(context_value);
        if (udp) {
            auto negotiated = prepare_http_connect_udp_response(std::move(head_response), d.parsed_.request_.protocol_version());
            if ((negotiated.index() != 0)) {
                throw std::invalid_argument("invalid CONNECT-UDP response metadata");
            }
            head_response = std::move(std::get<0>(negotiated));
        }
        const auto plan = [&]() -> std::variant<http1_response_head_plan, http_protocol_error> {
            if (!udp) {
                return prepare_http1_connect_response_head(head_response, d.parsed_.request_.protocol_version());
            }
            const auto upgrade = prepare_http1_connect_udp_response_head(head_response);
            if ((upgrade.index() != 0)) {
                return http_protocol_error(http_status::internal_server_error, "invalid CONNECT-UDP response head");
            }
            return std::get<0>(upgrade);
        }();
        if (plan.index() != 0) {
            throw std::get<1>(plan);
        }
        http_response_head_buffer head(std::pmr::polymorphic_allocator<char>(d.memory_.resource()));
        append_http1_response_head(head_response, head, std::get<0>(plan));
        context_access::mark_tunnel_handshake_started(context_value);
        const auto written = co_await async_asio<std::size_t>([&](auto handler) {
            asio::async_write(d.stream_, asio::buffer(head.view()), std::move(handler));
        });
        if (written.error_code()) {
            throw std::system_error(written.error_code(), "CONNECT head write");
        }
        if constexpr (requires { d.stream_.next_layer(); }) {
            tls_output.emplace(*d.stream_.native_handle(), d.stream_.next_layer(), d.base_route_services_.worker(), *d.memory_.resource());
            tls_output->start();
        }
        tunnel.emplace(http_socket_tunnel_transport<stream_type>(d.stream_, tls_output ? &*tls_output : nullptr),
            d.base_route_services_.worker(), *d.memory_.resource(), pending);
        co_await invoke_tunnel_handler(*tunnel, d.scanner_entry_, endpoint.handler(), context_value);
    };
    std::optional<http_response> buffered;
    std::exception_ptr failure;
    try {
        buffered = co_await d.routes_.dispatch_tunnel(d.parsed_.request_, resolved, d.request_memory_,
            make_callable_ref<void, context&>(establish_and_run), d.base_route_services_);
    } catch (...) {
        failure = std::current_exception();
    }
    if (tunnel) {
        co_await finish_tunnel_session(*tunnel, failure, d.options_.connection_failure_, d.base_route_services_.get_conn_info().remote().address(), d.scanner_entry_, endpoint.config().peer_transport_fin_timeout_);
    } else if (tls_output) {
        tls_output->abort();
    }
    if (tls_output) {
        co_await tls_output->join();
    }
    if (tunnel) {
        co_return std::nullopt;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (buffered) {
        d.response_ = std::move(*buffered);
        co_return http1_session_request_completion::make_buffered_closing(
            require_http1_final_response_commit(d.response_, d.parsed_.connection_plan_.require_close()));
    }
    co_return std::nullopt;
}

}  // namespace ruvia::detail
