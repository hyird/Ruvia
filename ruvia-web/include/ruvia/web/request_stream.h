#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/scoped_operation.h"
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
#include "ruvia/web/websocket.h"

namespace ruvia {

class request_stream;
class router;
enum class http2_error_code : std::uint32_t;
enum class http3_error_code : std::uint64_t;

enum class request_body_framing { none,
    known_length,
    chunked,
    multiplexed };

// 初始协议计划的快照；读取正文不会改变这些字段。
struct request_body_state final {
    request_body_framing framing_{request_body_framing::none};
    std::optional<std::uint64_t> content_length_{};
    // HTTP/1 与服务器合成的 push 请求不适用；HTTP/3 表示初始 HEADERS 随 FIN 结束流。
    std::optional<bool> initial_end_stream_{};
};

namespace detail {
struct request_stream_access;
class router_state;
class websocket_route_endpoint;
// Commits a tunnel request's success head and returns the established tunnel.
using request_stream_tunnel_accept = callback_ref<task<http_tunnel*>(http_status_code, http_tunnel_route_config)>;
// Negotiates against a validated endpoint the caller keeps alive until the
// returned task completes.
using request_stream_websocket_accept = callback_ref<task<websocket_accept_result>(const websocket_route_endpoint&)>;
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
// answers use the framework's default error representation. The host's
// server_config::cors_ and compression_ policies apply to every response, as
// to routed ones. No route, middleware, document root, rate limit or on_error
// handler takes part automatically. A callback may explicitly await
// router::dispatch() to apply its own frozen routes and middleware.
//
// The callback selects exactly one response: respond(), response().commit(),
// accept_tunnel(), accept_websocket(), or terminal reset()/abort_connection().
// respond() transfers a complete response to the server, which sends it after
// the callback returns; it does not commit output to the wire. Before wire commitment, callback failures
// replace the selected response with the framework's error answer. After wire
// commitment, a callback exception aborts the stream (HTTP/1: the connection)
// and reaches on_connection_failure. A callback returning without a response
// answers 500; a committed response stream not explicitly ended is ended.
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

    // Shares the state returned by the connection-open callback.
    [[nodiscard]] std::shared_ptr<void> connection_state() const noexcept {
        return context_.connection_state();
    }
    // Immediately aborts the entire connection, not only this HTTP/2 or HTTP/3
    // stream. Other in-flight requests on that connection are cancelled too.
    // This is a terminal choice; no response is required afterwards. A later
    // callback exception is still reported rather than treated as success.
    void abort_connection() noexcept {
        context_.abort_connection();
    }

    // 终结当前请求；HTTP/2、HTTP/3 只重置本流，HTTP/1 关闭连接。
    // 默认分别使用 CANCEL / H3_REQUEST_CANCELLED。正常返回不再自动生成响应；
    // 后续异常仍上报。显式协议错误码与当前协议不匹配时抛 invalid_argument。
    void reset();
    void reset(http2_error_code error);
    void reset(http3_error_code error);

    [[nodiscard]] request_body_state body_state() const noexcept;

    // The streamed request content. Its first read sends a pending
    // 100 Continue. Throws std::logic_error for a tunnel request, whose bytes
    // after the head belong to the tunnel.
    [[nodiscard]] body_reader& body() & RUVIA_LIFETIMEBOUND;
    body_reader& body() && = delete;

    // Available after body() reaches EOF; never replaces the initial fields.
    [[nodiscard]] std::span<const http_header> trailers() const noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const noexcept;

    // Sends a bodyless 1xx before the final head; copies fields before return.
    // `operation` follows context::inform().
    [[nodiscard]] task<void> inform(const http_interim_response_head& response, operation_options operation = {}) &;
    task<void> inform(const http_interim_response_head&, operation_options = {}) && = delete;
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
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(
        callable_type callable, operation_options options = {}) const {
        return context_.run_blocking(std::move(callable), std::move(options));
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

    // Edits the next file/static_file response or the head response().commit()
    // or an acceptance sends. Complete responses passed to respond() keep their
    // own metadata: respond() does not merge an unconsumed draft.
    [[nodiscard]] context_response_editor draft() & noexcept;
    context_response_editor draft() && = delete;

    // Reuses Context's file factory: conditional requests, HEAD and single or
    // multipart ranges retain their settled status and opened file handle.
    // Options are owned before return; static_file() borrows root until its
    // task completes. Blocking-pool, cancellation, timeout and identity checks
    // are the same as context::file()/static_file().
    [[nodiscard]] task<http_response> file(file_response_options options);
    [[nodiscard]] task<http_response> static_file(const static_root& root, static_file_response_options options = {});
    // A temporary root would be destroyed before the returned task reads it.
    task<http_response> static_file(const static_root&&, static_file_response_options = {}) = delete;

    // Transfers all response metadata and body ownership without sending yet.
    // The existing final-response driver sends buffers, files and multipart
    // ranges after the callback returns. Throws logic_error once any response
    // is selected or output committed. Failure before wire commitment still
    // replaces it with an error response, releasing its file/body resources.
    void respond(http_response response);

    // The streamed response: commit the head draft() describes, then write
    // the content and end it, with trailers where the protocol carries them.
    // A refused tunnel uses the same immediate, backpressured writer as any
    // response; HTTP/1.1 closes the connection after its framed response.
    // A successful CONNECT or 101 must instead use an accept operation.
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

    // Negotiates a WebSocket on an offered HTTP/1.1 Upgrade or extended
    // CONNECT through the server's normal handshake and session driver.
    // The cold task has no effects until awaited. On handshake rejection it
    // returns nullptr and selects the error response the server sends after
    // the callback returns. Any response/acceptance already selected throws
    // logic_error. The returned session is borrowed until the callback ends.
    [[nodiscard]] task<websocket_accept_result> accept_websocket(websocket_route_config config = {}) &;
    task<websocket_accept_result> accept_websocket(websocket_route_config = {}) && = delete;

    // Requested when the application stops and, for a request that is not a
    // tunnel request, when the client abandons it (HTTP/1: the connection
    // ends after the request content; HTTP/2 and HTTP/3: the stream is reset).
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept;

private:
    friend struct detail::request_stream_access;
    friend class router;
    friend class detail::router_state;

    request_stream(context& context_value, response_stream_writer& response,
        const detail::request_stream_tunnel_accept* accept_tunnel,
        const detail::request_stream_websocket_accept* accept_websocket = nullptr) noexcept
        : context_(context_value),
          response_(response),
          accept_tunnel_(accept_tunnel),
          accept_websocket_(accept_websocket) {
        operation_scope_.bind_worker(context_value.worker());
    }

    [[nodiscard]] task<http_tunnel*> accept_tunnel_impl(http_status_code status, http_tunnel_route_config config);
    [[nodiscard]] task<websocket_accept_result> accept_websocket_impl(websocket_route_config config);
    // A router's WebSocket route reuses its frozen endpoint.
    [[nodiscard]] task<websocket_accept_result> accept_websocket_endpoint(const detail::websocket_route_endpoint& endpoint);
    void require_unselected_response() const;

    context& context_;
    response_stream_writer& response_;
    // Null unless tunnel_requested().
    const detail::request_stream_tunnel_accept* accept_tunnel_;
    const detail::request_stream_websocket_accept* accept_websocket_;
    ::ruvia::operation_scope operation_scope_;
    bool routed_{false};
};

// application-owned handler of every listener_dispatch::request_stream
// listener, invoked on the worker that received the request; workers call the
// same target concurrently, so it must be thread-safe. The request ends when
// the returned task completes.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using request_stream_callback = detail::callback<task<void>(request_stream&)>;

}  // namespace ruvia
