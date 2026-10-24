#pragma once

#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/web/detail/callback_ref.h"

#include "context/context_access.h"
#include "context/context_services.h"
#include "http/http_tunnel_session.h"
#include "router/route_resolution.h"
#include "router/route_table.h"
#include "server/connection_scanner.h"
#include "server/http_server_options.h"
#include "server/worker_codecs.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/http_websocket_session.h"
#include "websocket/websocket_response_headers.h"

// The shared shape of every upgrade-capable route (CONNECT / CONNECT-UDP
// tunnels and WebSocket upgrades) across HTTP/1 and HTTP/2: run the
// application chain with a terminal that commits the upgrade and
// owns the resulting session; once a session exists it is always finished,
// otherwise a handler failure propagates and an uncommitted response falls
// back to the ordinary buffered tail.

namespace ruvia::detail {

[[nodiscard]] inline websocket_connection_options make_server_websocket_connection_options(
    const http_server_options& options, websocket_compression negotiated,
    const websocket_deflate_config& deflate = {}) noexcept {
    // Directions without context takeover borrow the worker's codecs, which
    // outlive every connection, so they allocate no codec state here.
    return websocket_connection_options{
        .compression_ = negotiated,
        .compression_level_ = deflate.compression_level_,
        .mem_level_ = deflate.mem_level_,
        .shared_deflate_ = &options.codecs_->websocket_deflate(),
    };
}

[[nodiscard]] inline http_response prepare_tunnel_upgrade_response_head(
    context& context_value, http_protocol_version version, bool udp) {
    auto head_response = context_access::streaming_head(context_value);
    if (udp) {
        auto negotiated = prepare_http_connect_udp_response(std::move(head_response), version);
        if (negotiated.index() != 0) {
            throw std::invalid_argument("invalid CONNECT-UDP response metadata");
        }
        head_response = std::move(std::get<0>(negotiated));
    }
    return head_response;
}

// `establish(context&) -> task<void>` commits the upgrade response head,
// emplaces `session` and runs the route's stream handler on it.
// `finish(session_type&, std::exception_ptr) -> task<void>` closes an
// established session and reports a handler failure; it always runs once a
// session exists, before any handler exception is surfaced.
// Returns a buffered response only when the application answered without
// upgrading; an established or abandoned upgrade returns std::nullopt.
template <typename session_type, typename establish_type, typename finish_type>
[[nodiscard]] task<std::optional<http_response>> run_http_upgrade_route(
    const route_table& routes, const http_request& request, const resolved_route& resolved, request_memory& memory,
    context_services services, std::optional<session_type>& session, establish_type& establish,
    finish_type& finish) {
    const auto terminal = callback_access::bind<task<void>(context&)>(establish);
    std::optional<http_response> buffered;
    std::exception_ptr failure;
    try {
        buffered = co_await routes.dispatch_upgrade(request, resolved, memory, terminal, std::move(services));
    } catch (...) {
        failure = std::current_exception();
    }
    if (session.has_value()) {
        co_await finish(*session, failure);
        co_return std::nullopt;
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    co_return buffered;
}

template <typename transport_type, typename commit_upgrade_type>
struct tunnel_upgrade_session_operation final {
    using session_type = http_tunnel_session<transport_type>;

    const http_request& request_;
    const resolved_route& resolved_;
    const tunnel_route_endpoint& endpoint_;
    const context_services& services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    std::pmr::memory_resource& inbound_;
    bool udp_;
    std::string_view pending_;
    commit_upgrade_type& commit_upgrade_;
    std::optional<session_type>& session_;

    [[nodiscard]] task<void> operator()(context& context_value) {
        const auto head_response =
            prepare_tunnel_upgrade_response_head(context_value, request_.protocol_version(), udp_);
        std::optional<transport_type> transport;
        if constexpr (requires { commit_upgrade_(context_value, head_response).operator co_await(); }) {
            transport.emplace(co_await commit_upgrade_(context_value, head_response));
        } else {
            transport.emplace(commit_upgrade_(context_value, head_response));
        }
        try {
            session_.emplace(
                std::move(*transport), services_.worker(), *context_value.pool(), inbound_, pending_);
        } catch (...) {
            transport->abort();
            throw;
        }
        co_await invoke_tunnel_handler(
            *session_, scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_tunnel_session(session_value, std::move(failure),
            options_.connection_failure_, services_.get_conn_info().remote().address(),
            scanner_entry_, endpoint_.config().peer_transport_fin_timeout_);
    }
};

template <typename transport_type, typename commit_upgrade_type>
[[nodiscard]] task<std::optional<http_response>> execute_tunnel_upgrade_session(
    const route_table& routes, const http_request& request, const resolved_route& resolved,
    request_memory& memory, context_services services, const http_server_options& options,
    connection_scanner::entry& scanner_entry, std::pmr::memory_resource& inbound, bool udp,
    std::string_view pending, commit_upgrade_type commit_upgrade) {
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    using operation_type = tunnel_upgrade_session_operation<transport_type, commit_upgrade_type>;
    std::optional<typename operation_type::session_type> session;
    operation_type operation{
        .request_ = request,
        .resolved_ = resolved,
        .endpoint_ = endpoint,
        .services_ = services,
        .options_ = options,
        .scanner_entry_ = scanner_entry,
        .inbound_ = inbound,
        .udp_ = udp,
        .pending_ = pending,
        .commit_upgrade_ = commit_upgrade,
        .session_ = session,
    };
    co_return co_await run_http_upgrade_route(
        routes, request, resolved, memory, services, session, operation, operation);
}

template <typename stream_type>
class websocket_upgrade_commit final {
public:
    static constexpr bool native_stream = server_websocket_connection<stream_type>::native_stream;
    using stream_argument_type = typename server_websocket_connection<stream_type>::stream_argument_type;

    websocket_upgrade_commit(stream_argument_type stream, websocket_compression compression) noexcept
        requires(native_stream)
        : stream_(&stream),
          compression_(compression) {}

    websocket_upgrade_commit(stream_argument_type stream, websocket_compression compression) noexcept
        requires(!native_stream)
        : stream_(std::move(stream)),
          compression_(compression) {}

    [[nodiscard]] stream_argument_type release_stream() noexcept {
        if constexpr (native_stream) {
            return *stream_;
        } else {
            return std::move(stream_);
        }
    }

    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

private:
    std::conditional_t<native_stream, stream_type*, stream_type> stream_;
    websocket_compression compression_{};
};

template <typename stream_type, typename commit_upgrade_type>
struct websocket_upgrade_session_operation final {
    using session_type = server_websocket_connection<stream_type>;

    const resolved_route& resolved_;
    const websocket_route_endpoint& websocket_endpoint_;
    const context_services& services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    std::pmr::memory_resource& inbound_;
    std::string_view pending_frames_;
    commit_upgrade_type& commit_upgrade_;
    std::optional<session_type>& session_;

    [[nodiscard]] task<void> operator()(context& context_value) {
        const auto response_headers_value = websocket_response_headers(context_value);
        std::optional<websocket_upgrade_commit<stream_type>> committed;
        if constexpr (requires { commit_upgrade_(context_value, response_headers_value).operator co_await(); }) {
            committed = co_await commit_upgrade_(context_value, response_headers_value);
        } else {
            committed.emplace(commit_upgrade_(context_value, response_headers_value));
        }
        if (!committed) {
            co_return;
        }
        session_.emplace(committed->release_stream(), services_.worker(), scanner_entry_,
            websocket_endpoint_.lifecycle(),
            protocol_byte_limit::limited(options_.server_.max_websocket_message_bytes_),
            &inbound_, pending_frames_,
            make_server_websocket_connection_options(
                options_, committed->compression(), websocket_endpoint_.deflate()));
        co_await invoke_websocket_handler(
            *session_, scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_websocket_session(session_value, std::move(failure),
            options_.connection_failure_, services_.get_conn_info().remote().address());
    }
};

template <typename stream_type, typename commit_upgrade_type>
[[nodiscard]] task<std::optional<http_response>> execute_websocket_upgrade_session(
    const route_table& routes, const http_request& request, const resolved_route& resolved,
    request_memory& memory, context_services services, const http_server_options& options,
    connection_scanner::entry& scanner_entry, std::pmr::memory_resource& inbound,
    std::string_view pending_frames, commit_upgrade_type commit_upgrade) {
    const auto& websocket_endpoint = *resolved.route().endpoint().websocket();
    using operation_type = websocket_upgrade_session_operation<stream_type, commit_upgrade_type>;
    std::optional<typename operation_type::session_type> session;
    operation_type operation{
        .resolved_ = resolved,
        .websocket_endpoint_ = websocket_endpoint,
        .services_ = services,
        .options_ = options,
        .scanner_entry_ = scanner_entry,
        .inbound_ = inbound,
        .pending_frames_ = pending_frames,
        .commit_upgrade_ = commit_upgrade,
        .session_ = session,
    };
    co_return co_await run_http_upgrade_route(
        routes, request, resolved, memory, services, session, operation, operation);
}

}  // namespace ruvia::detail
