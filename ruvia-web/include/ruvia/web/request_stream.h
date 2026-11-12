#pragma once

#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/callback.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/streaming.h"

namespace ruvia {

class request_stream;

namespace detail {
struct request_stream_access;
// Commits a tunnel request's success head and returns the established tunnel.
using request_stream_tunnel_accept = callback_ref<task<http_tunnel*>(http_status_code, http_tunnel_route_config)>;
using request_stream_callback_ref = callback_ref<task<void>(request_stream&)>;
}  // namespace detail

// One HTTP request a listener_dispatch::request_stream listener received --
// over HTTP/1.1, HTTP/2 or HTTP/3 -- handed to application::on_request_stream
// in place of the application's routes. The server owns it and it is valid
// only while the callback task runs; every member must be used on the worker
// running that task.
//
// The server still frames and validates the request, answers a target this
// connection cannot carry with 421, an unsupported Expect with 417 and a
// declared body over server_config::max_stream_body_bytes_ with 413, applies
// flow control, the server timeouts and the HTTP/3 0-RTT 425 rule; these
// answers use the framework's default error representation. No route,
// middleware, document root, CORS policy, response compression, rate limit
// or on_error handler takes part.
//
// The callback commits exactly one response: through response(), or for a
// tunnel request through accept_tunnel(). Once it is committed, an exception
// from the callback aborts the stream (HTTP/1: the connection) and reaches
// on_connection_failure. Before that, an http_error answers with its status,
// a request body failure with its 4xx, and any other exception -- or a
// callback that returns without committing -- with 500 after reaching
// on_connection_failure. A committed response stream the callback did not
// end is ended when it returns.
class request_stream final {
public:
    request_stream(const request_stream&) = delete;
    request_stream& operator=(const request_stream&) = delete;
    request_stream(request_stream&&) = delete;
    request_stream& operator=(request_stream&&) = delete;

    // Method, target and header fields; the body arrives through body().
    [[nodiscard]] const http_request& request() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_request& request() const&& = delete;

    // conn_info::id() names the connection, as on routes listeners.
    [[nodiscard]] const conn_info& conn() const& noexcept RUVIA_LIFETIMEBOUND;
    const conn_info& conn() const&& = delete;

    // The streamed request content. Its first read sends a pending
    // 100 Continue. Throws std::logic_error for a tunnel request, whose bytes
    // after the head belong to the tunnel.
    [[nodiscard]] body_reader& body() & RUVIA_LIFETIMEBOUND;
    body_reader& body() && = delete;

    // Available after body() reaches EOF; never replaces the initial fields.
    [[nodiscard]] std::span<const http_header> trailers() const noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const noexcept;

    // Sends a bodyless 1xx before the final head; copies fields before return.
    [[nodiscard]] task<void> inform(const http_interim_response_head& response);
    [[nodiscard]] bool early_data() const noexcept;
    [[nodiscard]] bool push(http_push_request_view request);
    void advertise_origins(std::span<const std::string_view> origins);
    void advertise_alternative_service(std::string_view value);

    [[nodiscard]] const worker_handle& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return context_.worker();
    }
    const worker_handle& worker() const&& = delete;

    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return context_.worker_state<state_type>();
    }

    // Bound to this request's cancellation, including client abandonment.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(callable_type callable) const {
        return context_.run_blocking(std::move(callable));
    }

    template <typename rep_type, typename period_type, typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(
        std::chrono::duration<rep_type, period_type> timeout, callable_type callable) const {
        return context_.run_blocking(timeout, std::move(callable));
    }

    [[nodiscard]] std::pmr::memory_resource* arena() const noexcept {
        return context_.arena();
    }
    [[nodiscard]] std::pmr::memory_resource* pool() const noexcept {
        return context_.pool();
    }
    [[nodiscard]] const ruvia::env& env() const noexcept {
        return context_.env();
    }
    [[nodiscard]] http_client_handle client() const;
    [[nodiscard]] http_client_handle client(std::string_view alias) const;
#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_handle db() const;
    [[nodiscard]] db_handle db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_handle redis() const;
    [[nodiscard]] redis_handle redis(std::string_view alias) const;
#endif

    // Edits the head the next response().commit() or accept_tunnel() sends:
    // status, header fields and cookies.
    [[nodiscard]] context_response_editor draft() & noexcept;
    context_response_editor draft() && = delete;

    // The streamed response: commit the head draft() describes, then write
    // the content and end it, with trailers where the protocol carries them.
    // A tunnel request's response is buffered until the callback returns, then
    // sent without trailers (non-empty ones throw std::logic_error) and, over
    // HTTP/1.1, followed by closing the connection; a 2xx to CONNECT can only
    // be sent by accept_tunnel().
    [[nodiscard]] response_stream_writer& response() & noexcept RUVIA_LIFETIMEBOUND;
    response_stream_writer& response() && = delete;

    // Whether accept_tunnel() can establish a tunnel: a CONNECT or extended
    // CONNECT (RFC 9110 §9.3.6, RFC 8441, RFC 9220), or an HTTP/1.1 request
    // without content offering Upgrade (RFC 9110 §7.8).
    [[nodiscard]] bool tunnel_requested() const noexcept;

    // Commits the success head draft() describes and returns the byte stream
    // that follows it: `status` must be 2xx for CONNECT and 101 (Switching
    // Protocols) for an HTTP/1.1 Upgrade, which names the response's Upgrade
    // field or else the first protocol the client offered (std::invalid_argument
    // otherwise). Throws std::logic_error unless tunnel_requested(), or once a
    // response or tunnel was committed. The tunnel stays valid until the
    // callback returns; the server then ends its send direction and drains the
    // peer's for up to http_tunnel_route_config::peer_transport_fin_timeout_.
    // config.datagrams_ explicitly enables Capsule/HTTP Datagram semantics
    // for an upgrade protocol; plain CONNECT rejects it. CONNECT-UDP enables
    // them automatically and uses the protocol's existing handshake checks.
    [[nodiscard]] task<http_tunnel*> accept_tunnel(http_status_code status = http_status::ok, http_tunnel_route_config config = {}) &;
    task<http_tunnel*> accept_tunnel(http_status_code = http_status::ok, http_tunnel_route_config = {}) && = delete;

    // Requested when the application stops and, for a request that is not a
    // tunnel request, when the client abandons it (HTTP/1: the connection
    // ends after the request content; HTTP/2 and HTTP/3: the stream is reset).
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept;

private:
    friend struct detail::request_stream_access;

    request_stream(context& context_value, response_stream_writer& response,
        const detail::request_stream_tunnel_accept* accept_tunnel) noexcept
        : context_(context_value),
          response_(response),
          accept_tunnel_(accept_tunnel) {}

    context& context_;
    response_stream_writer& response_;
    // Null unless tunnel_requested().
    const detail::request_stream_tunnel_accept* accept_tunnel_;
};

// application-owned handler of every listener_dispatch::request_stream
// listener, invoked on the worker that received the request; workers call the
// same target concurrently, so it must be thread-safe. The request ends when
// the returned task completes.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using request_stream_callback = detail::callback<task<void>(request_stream&)>;

}  // namespace ruvia
