#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "blocking_pool.h"
#include "borrowed_text.h"
#include "callback_ref.h"
#include "context.h"
#include "http2_push.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "http_status.h"
#include "http_tunnel.h"
#include "move_only_function.h"
#include "operation_options.h"
#include "operation_scope.h"
#include "static_root.h"
#include "stop_token.h"
#include "streaming.h"
#include "task.h"
#include "websocket.h"
#include "worker_ref.h"

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
class operation_lane_lease;
class websocket_route_endpoint;
// Commits a tunnel request's success head and returns the established tunnel.
using request_stream_tunnel_accept = callback_ref<task<std::reference_wrapper<http_tunnel>>(http_status_code, http_tunnel_route_config)>;
// Negotiates against a validated endpoint the caller keeps alive until the
// returned task completes.
using request_stream_websocket_accept = callback_ref<task<websocket_accept_result>(const websocket_route_endpoint&)>;
}  // namespace detail

// One HTTP request a listener_dispatch::request_stream listener received over
// HTTP/1.1, HTTP/2 or HTTP/3, handed to its host's request callback instead of
// application routes. The host owns it only while the callback task runs;
// every member runs on that callback's worker.
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
// The callback selects exactly one response: respond(), stream().commit(),
// accept_tunnel(), accept_websocket(), or terminal reset()/abort_connection().
// respond() transfers a complete response to the server, which sends it after
// the callback returns; it does not commit output to the wire. Before wire commitment, callback failures
// replace the selected response with the framework's error answer. After wire
// commitment, a callback exception aborts the stream (HTTP/1: the connection)
// and reaches connection_failure_. A callback returning without a response
// answers 500; a committed response stream not explicitly ended is ended.
// Worker resources come from worker_services_access and the connection,
// streaming and response-builder members from request_exchange, as on
// context, bound to this request's cancellation, including client abandonment.
class request_stream final : public worker_services_access<request_stream>, public request_exchange<request_stream> {
public:
    request_stream(const request_stream&) = delete;
    request_stream& operator=(const request_stream&) = delete;
    request_stream(request_stream&&) = delete;
    request_stream& operator=(request_stream&&) = delete;

    // Method, target and header fields; the body arrives through request_body().
    [[nodiscard]] const http_request& request() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_request& request() const&& = delete;

    // 终结当前请求；HTTP/2、HTTP/3 只重置本流，HTTP/1 关闭连接。
    // 默认分别使用 CANCEL / H3_REQUEST_CANCELLED。正常返回不再自动生成响应；
    // 后续异常仍上报。显式协议错误码与当前协议不匹配时抛 invalid_argument。
    void reset() &;
    void reset() && = delete;
    void reset(http2_error_code error) &;
    void reset(http2_error_code) && = delete;
    void reset(http3_error_code error) &;
    void reset(http3_error_code) && = delete;

    [[nodiscard]] request_body_state body_state() const noexcept;

    // The streamed request content. Its first read sends a pending
    // 100 Continue. Throws std::logic_error for a tunnel request, whose bytes
    // after the head belong to the tunnel.
    [[nodiscard]] body_reader& request_body() & RUVIA_LIFETIMEBOUND;
    body_reader& request_body() && = delete;

    // Transfers all response metadata and body ownership without sending yet.
    // The existing final-response driver sends buffers, files and multipart
    // ranges after the callback returns. Throws logic_error once any response
    // is selected or output committed. Failure before wire commitment still
    // replaces it with an error response, releasing its file/body resources.
    void respond(http_response&& response) &;
    void respond(http_response&&) && = delete;

    // Whether accept_tunnel() can establish a tunnel: a CONNECT or extended
    // CONNECT (RFC 9110 §9.3.6, RFC 8441, RFC 9220), or an HTTP/1.1 request
    // without content offering Upgrade (RFC 9110 §7.8).
    [[nodiscard]] bool tunnel_requested() const noexcept;

    // Commits the success head (policy() included) and returns the byte stream
    // that follows it: `status` must be 2xx for CONNECT and 101 (Switching
    // Protocols) for an HTTP/1.1 Upgrade, which names the response's Upgrade
    // field or else the first protocol the client offered (std::invalid_argument
    // otherwise). Throws std::logic_error unless tunnel_requested(), or once a
    // response or tunnel was committed. The tunnel stays valid until the
    // callback returns; the server then ends its send direction and drains the
    // peer's for up to http_tunnel_route_config::peer_transport_fin_timeout_;
    // config.idle_timeout_ aborts it once no tunnel byte moves for that long.
    // config.datagrams_ explicitly enables Capsule/HTTP Datagram semantics
    // for an upgrade protocol; plain CONNECT rejects it. CONNECT-UDP enables
    // them automatically and uses the protocol's existing handshake checks.
    [[nodiscard]] task<std::reference_wrapper<http_tunnel>> accept_tunnel(http_status_code status = http_status::ok, http_tunnel_route_config config = {}) &;
    task<std::reference_wrapper<http_tunnel>> accept_tunnel(http_status_code = http_status::ok, http_tunnel_route_config = {}) && = delete;

    // Negotiates a WebSocket on an offered HTTP/1.1 Upgrade or extended
    // CONNECT through the server's normal handshake and session driver.
    // The cold task has no effects until awaited. On handshake rejection it
    // returns a rejection and selects the error response the server sends after
    // the callback returns. Any response/acceptance already selected throws
    // logic_error. The returned session is borrowed until the callback ends.
    [[nodiscard]] task<websocket_accept_result> accept_websocket(websocket_route_config config = {}) &;
    task<websocket_accept_result> accept_websocket(websocket_route_config = {}) && = delete;

private:
    friend struct detail::request_stream_access;
    friend class worker_services_access<request_stream>;
    friend class request_exchange<request_stream>;
    // http_client_response::pipe_to() binds its forward to operation_scope_.
    friend class http_client_response;
    friend class router;
    friend class detail::router_state;

    request_stream(context& context_value, response_stream_writer& response, const detail::request_stream_tunnel_accept* accept_tunnel, const detail::request_stream_websocket_accept* accept_websocket = nullptr) noexcept :
        context_(context_value),
        response_(response),
        accept_tunnel_(accept_tunnel),
        accept_websocket_(accept_websocket) {
        operation_scope_.bind_worker(context_value.worker());
    }

    [[nodiscard]] detail::worker_services services() const noexcept {
        return context_.services();
    }

    [[nodiscard]] context& exchange_context() const noexcept {
        return context_;
    }

    [[nodiscard]] task<std::reference_wrapper<http_tunnel>> accept_tunnel_impl(http_status_code status, http_tunnel_route_config config);
    [[nodiscard]] task<websocket_accept_result> accept_websocket_impl(websocket_route_config config);
    [[nodiscard]] detail::operation_lane_lease claim_websocket_accept();
    void require_unselected_response() const;

    context& context_;
    response_stream_writer& response_;
    // Null unless tunnel_requested().
    const detail::request_stream_tunnel_accept* accept_tunnel_;
    const detail::request_stream_websocket_accept* accept_websocket_;
    detail::operation_scope operation_scope_;
    bool routed_{false};
};

// Host-owned handler of every listener_dispatch::request_stream listener.
// application may share one target across workers; http_server_group builds
// a target per worker. The request ends when its returned task completes.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using request_stream_callback = move_only_function<task<void>(request_stream&)>;

}  // namespace ruvia
