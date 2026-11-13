#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/context.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/streaming.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_framing.h"
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "ruvia/http/http3_connection.h"
#endif

namespace ruvia {

namespace detail {
template <typename stream_type>
class server_connection_session;
class connection_lifecycle;

// What the worker knows of an accepted connection. Every view borrows
// connection-owned storage that outlives the server_connection.
struct server_connection_fields final {
    struct tls_type final {
        std::optional<std::string_view> client_certificate_subject_;
        std::string_view alpn_;
        std::string_view server_name_;
    };

    std::uint64_t id_;
    std::string_view local_address_;
    std::uint16_t local_port_;
    std::string_view remote_address_;
    std::uint16_t remote_port_;
    std::optional<tls_type> tls_;
    const std::shared_ptr<void>* state_{};
    connection_lifecycle* lifecycle_{};
};
}  // namespace detail

// One connection a listener_dispatch::connection listener accepted, handed to
// its host's connection callback: the decrypted byte stream after a TLS
// handshake, the socket's bytes otherwise. The host owns it only while the
// callback task runs; every member runs on that callback's worker.
//
// One read and one output operation (write or finish) may run at a time,
// concurrently with each other. The connection is subject to the server's
// limits: it holds a max_connections_per_worker_ slot (and is never evicted to
// admit another connection), its read buffer counts against the inbound
// buffer budgets, idle_timeout_ closes it after that long without a read or
// write completing, write_timeout_ closes it when a write makes no progress
// for that long, and host shutdown closes it at once and requests its
// stop_token(). A closed connection fails its pending and later operations
// with std::system_error.
class server_connection final {
public:
    server_connection(const server_connection&) = delete;
    server_connection& operator=(const server_connection&) = delete;
    server_connection(server_connection&&) = delete;
    server_connection& operator=(server_connection&&) = delete;

    // conn_info::connection_id() of this connection; equal to connection_id() of its
    // on_tls_client_hello records.
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return id_;
    }

    // The server address the connection reached.
    [[nodiscard]] constexpr ip_endpoint local() const noexcept {
        return local_;
    }

    [[nodiscard]] constexpr ip_endpoint remote() const noexcept {
        return remote_;
    }

    // Null for a cleartext listener.
    [[nodiscard]] constexpr const tls_connection_transport* tls() const& noexcept RUVIA_LIFETIMEBOUND {
        return tls_.has_value() ? &*tls_ : nullptr;
    }
    const tls_connection_transport* tls() const&& = delete;

    // Requested when the application stops.
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept {
        return stop_token_;
    }

    // Borrowed for this callback; copy the handle to retain its dispatcher.
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return worker_;
    }
    const worker_ref& worker() const&& = delete;
    [[nodiscard]] std::shared_ptr<void> connection_state() const noexcept {
        return state_ == nullptr ? std::shared_ptr<void>{} : *state_;
    }

    // The next bytes the peer sent, at most 4 KiB; std::nullopt once the peer
    // ended its send direction (TCP FIN or TLS close_notify, or TCP closed
    // without close_notify). The view stays valid until the next read starts.
    // Reading past the inbound buffer budget throws std::bad_alloc. A timeout
    // or stop in `operation` ends only this read with std::system_error
    // (timed_out or operation_canceled); no input is lost and reading may
    // continue. A non-positive timeout throws std::invalid_argument.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    // Copies `bytes` before returning, then sends them once awaited. A timeout
    // or stop in `operation` of write() or finish() aborts the connection (a
    // partial write cannot be withdrawn) and throws std::system_error
    // (timed_out or operation_canceled); a stop requested before the operation
    // starts sends nothing. The server's write_timeout_ still applies.
    [[nodiscard]] task<void> write(std::string_view bytes, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& bytes, operation_options operation = {}) & {
        return write_owned(std::move(bytes), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;
    // Ends the send direction: TCP shuts down its send side, TLS sends
    // close_notify. Reading remains available until the peer ends its own.
    // Writing afterwards throws std::logic_error; finishing again does nothing.
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    // Closes the connection at once, failing operations still running.
    void abort() & noexcept;
    void abort() && = delete;

private:
    template <typename stream_type>
    friend class detail::server_connection_session;
    using read_type = task<std::optional<std::span<const std::byte>>> (*)(void*, operation_options);
    using write_type = task<void> (*)(void*, std::string_view, operation_options);
    using finish_type = task<void> (*)(void*, operation_options);
    using abort_type = void (*)(void*) noexcept;

    server_connection(const detail::server_connection_fields& fields, const ::ruvia::stop_token& stop_token_value,
        const worker_ref& worker_value, void* target, read_type read, write_type write, finish_type finish_value,
        abort_type abort_value) noexcept;
    void require_active() const;
    [[nodiscard]] task<void> write_owned(std::string&& bytes, operation_options operation);

    const worker_ref& worker_;
    void* target_;
    read_type read_;
    write_type write_;
    finish_type finish_;
    abort_type abort_;
    std::uint64_t id_;
    ip_endpoint local_;
    ip_endpoint remote_;
    std::optional<tls_connection_transport> tls_;
    ::ruvia::stop_token stop_token_;
    const std::shared_ptr<void>* state_;
    bool read_active_{false};
    bool write_active_{false};
    bool send_ended_{false};
    ::ruvia::operation_scope operations_;
};

namespace detail {
using connection_callback_ref = callback_ref<task<void>(server_connection&)>;
}  // namespace detail

// Host-owned handler of every listener_dispatch::connection listener.
// application may share one target across workers; http_server_group builds
// a target per worker through its callback factory. When the returned
// task completes the server ends the connection: TLS sends close_notify
// unless the handler finished or aborted it, then the socket closes. A task
// that throws aborts the connection; its exception reaches
// on_connection_failure unless it is a peer reset or close, or I/O the
// server cancelled (timeouts or host shutdown).
// 空函数指针与目标析构重入的契约同 access_log_callback。
using connection_callback = move_only_function<task<void>(server_connection&)>;

}  // namespace ruvia

namespace ruvia {

namespace detail {
struct server_observation_access;
}  // namespace detail

// One ClientHello a server TLS connection received, passed to
// on_tls_client_hello before the server selects its certificate. The record
// and every view are valid only for the callback.
class tls_client_hello_record final {
public:
    // conn_info::connection_id() of every request the connection carries.
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }

    // The server address the connection reached. For HTTP/3 it is the address
    // the listener's UDP socket is bound to.
    [[nodiscard]] constexpr ip_endpoint local() const noexcept {
        return local_;
    }

    [[nodiscard]] constexpr ip_endpoint remote() const noexcept {
        return remote_;
    }

    // True for the TLS of an HTTP/3 (QUIC) connection, false for TLS over TCP.
    [[nodiscard]] constexpr bool quic() const noexcept {
        return quic_;
    }

    // The complete handshake message (RFC 9846 §4.1.2, RFC 5246 §7.4.1.2):
    // msg_type 1, the 3-byte length and the body, reassembled from however
    // many records or CRYPTO frames carried it. The second ClientHello after a
    // HelloRetryRequest is delivered again, as its own record.
    [[nodiscard]] constexpr std::span<const std::byte> message() const noexcept {
        return message_;
    }

private:
    friend struct detail::server_observation_access;

    struct fields_type final {
        std::uint64_t connection_id_;
        std::string_view local_address_;
        std::uint16_t local_port_;
        std::string_view remote_address_;
        std::uint16_t remote_port_;
        bool quic_;
        std::span<const std::byte> message_;
    };

    explicit tls_client_hello_record(const fields_type& fields) noexcept
        : connection_id_(fields.connection_id_),
          local_(ip_address::parse(fields.local_address_).value_or(ip_address{}), fields.local_port_),
          remote_(ip_address::parse(fields.remote_address_).value_or(ip_address{}), fields.remote_port_),
          message_(fields.message_),
          quic_(fields.quic_) {}

    std::uint64_t connection_id_;
    ip_endpoint local_;
    ip_endpoint remote_;
    std::span<const std::byte> message_;
    bool quic_;
};

// One read of decrypted HTTP/2 connection input, passed to on_http2_input
// before the server parses it. The record and its view are valid only for the
// callback.
class http2_input_record final {
public:
    // conn_info::connection_id() of every request the connection carries.
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }

    // The connection's inbound bytes in wire order: the records of one
    // connection concatenate to its input from the client connection preface
    // (RFC 9113 §3.4) on. Boundaries are read boundaries, not frame boundaries.
    [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }

private:
    friend struct detail::server_observation_access;

    constexpr http2_input_record(std::uint64_t connection_id, std::span<const std::byte> bytes) noexcept
        : connection_id_(connection_id),
          bytes_(bytes) {}

    std::uint64_t connection_id_;
    std::span<const std::byte> bytes_;
};

#if defined(RUVIA_ENABLE_HTTP2)
// Typed facts produced by the one protocol parser/HPACK decoder, not TCP read
// boundaries. Records and decoded field views live only for the callback.
// observation_sequence_ counts parser-exposed frame headers, not transport reads.
// Frames are observed before processing their final payload fragment; headers
// are observed at decoder begin/field/end points before HTTP validation.
// Observers cannot change input, cancel validation, or imply frame acceptance.
class http2_frame_record final {
public:
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }
    [[nodiscard]] const http2_frame_metadata& metadata() const noexcept {
        return metadata_;
    }

private:
    friend struct detail::server_observation_access;
    constexpr http2_frame_record(std::uint64_t connection_id, const http2_frame_metadata& metadata) noexcept
        : connection_id_(connection_id),
          metadata_(metadata) {}
    std::uint64_t connection_id_;
    const http2_frame_metadata& metadata_;
};

class http2_headers_record final {
public:
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }
    [[nodiscard]] const http2_header_metadata& metadata() const noexcept {
        return metadata_;
    }

private:
    friend struct detail::server_observation_access;
    constexpr http2_headers_record(std::uint64_t connection_id, const http2_header_metadata& metadata) noexcept
        : connection_id_(connection_id),
          metadata_(metadata) {}
    std::uint64_t connection_id_;
    const http2_header_metadata& metadata_;
};
#endif
#if defined(RUVIA_ENABLE_HTTP3)
class http3_frame_record final {
public:
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }
    [[nodiscard]] const http3_frame_metadata& metadata() const noexcept {
        return metadata_;
    }

private:
    friend struct detail::server_observation_access;
    constexpr http3_frame_record(std::uint64_t connection_id, const http3_frame_metadata& metadata) noexcept
        : connection_id_(connection_id),
          metadata_(metadata) {}
    std::uint64_t connection_id_;
    const http3_frame_metadata& metadata_;
};

class http3_field_record final {
public:
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }
    [[nodiscard]] const http3_field_metadata& metadata() const noexcept {
        return metadata_;
    }

private:
    friend struct detail::server_observation_access;
    constexpr http3_field_record(std::uint64_t connection_id, const http3_field_metadata& metadata) noexcept
        : connection_id_(connection_id),
          metadata_(metadata) {}
    std::uint64_t connection_id_;
    const http3_field_metadata& metadata_;
};
#endif

namespace detail {
using tls_client_hello_callback_ref = callback_ref<void(const tls_client_hello_record&) noexcept>;
using http2_input_callback_ref = callback_ref<void(const http2_input_record&) noexcept>;
#if defined(RUVIA_ENABLE_HTTP2)
using http2_frame_callback_ref = callback_ref<void(const http2_frame_record&) noexcept>;
using http2_headers_callback_ref = callback_ref<void(const http2_headers_record&) noexcept>;
#endif
#if defined(RUVIA_ENABLE_HTTP3)
using http3_frame_callback_ref = callback_ref<void(const http3_frame_record&) noexcept>;
using http3_field_callback_ref = callback_ref<void(const http3_field_record&) noexcept>;
#endif
}  // namespace detail

// application-owned observers, invoked synchronously on the worker thread that
// owns the connection; workers call the same target concurrently, so it must
// be thread-safe. Connections only borrow an allocation-free callback_ref.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using tls_client_hello_callback = move_only_function<void(const tls_client_hello_record&) noexcept>;
using http2_input_callback = move_only_function<void(const http2_input_record&) noexcept>;
#if defined(RUVIA_ENABLE_HTTP2)
using http2_frame_callback = move_only_function<void(const http2_frame_record&) noexcept>;
using http2_headers_callback = move_only_function<void(const http2_headers_record&) noexcept>;
#endif
#if defined(RUVIA_ENABLE_HTTP3)
using http3_frame_callback = move_only_function<void(const http3_frame_record&) noexcept>;
using http3_field_callback = move_only_function<void(const http3_field_record&) noexcept>;
#endif

}  // namespace ruvia

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

    // conn_info::connection_id() names the connection, as on routes listeners.
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

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return context_.worker();
    }
    const worker_ref& worker() const&& = delete;

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

    [[nodiscard]] const ruvia::env& env() const noexcept {
        return context_.env();
    }
    [[nodiscard]] http_client_ref client() const;
    [[nodiscard]] http_client_ref client(std::string_view alias) const;
#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_session db() const;
    [[nodiscard]] db_session db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_session redis() const;
    [[nodiscard]] redis_session redis(std::string_view alias) const;
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
    // http_client_response::pipe_to() binds its forward to operation_scope_.
    friend class http_client_response;
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

// Host-owned handler of every listener_dispatch::request_stream listener.
// application may share one target across workers; http_server_group builds
// a target per worker. The request ends when its returned task completes.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using request_stream_callback = move_only_function<task<void>(request_stream&)>;

}  // namespace ruvia

namespace ruvia {

namespace detail {
class http_server_state;
class http_server_group_state;
}  // namespace detail

struct http_server_callbacks final {
    request_stream_callback request_stream_;
    connection_callback connection_;
    access_log_callback access_;
    connection_failure_callback connection_failure_;
    tls_client_hello_callback tls_client_hello_;
    http2_input_callback http2_input_;
#if defined(RUVIA_ENABLE_HTTP2)
    http2_frame_callback http2_frame_;
    http2_headers_callback http2_headers_;
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    http3_frame_callback http3_frame_;
    http3_field_callback http3_field_;
#endif
    connection_admission_callback connection_admission_;
    connection_open_callback connection_open_;
    connection_close_callback connection_close_;
};

// One inbound service host on a caller-owned event loop. No application,
// controller registry, business thread or process signal handler is involved.
// Windows listeners reuse the dedicated native acceptor. Runtime scheduling
// fields in server_config belong to the caller's loop; limits apply here to
// this one worker. The host owns callbacks until every session has retired;
// they are released on the loop once shutdown has joined every session.
class http_server final {
public:
    http_server(event_loop loop, http_server_callbacks callbacks, server_config config = {});
    ~http_server();
    http_server(const http_server&) = delete;
    http_server& operator=(const http_server&) = delete;
    http_server(http_server&&) = delete;
    http_server& operator=(http_server&&) = delete;

    // Owner loop, before start only: stages owned configuration without
    // loading credentials; start() loads and checks it, dispatch_ included.
    // Runtime additions use add_listener().
    http_server& listen(listen_config config) &;
    http_server& listen(listen_config) && = delete;
    [[nodiscard]] task<prepared_listener> prepare_listener(listen_config config) &;
    task<prepared_listener> prepare_listener(listen_config) && = delete;
    // 整批准备并共享一次提交/回滚；所有配置均就绪后才返回。
    // 地址/TLS 配置在调用时复制，继承 socket 的所有权同时转入，包括冷任务。
    [[nodiscard]] task<prepared_listener> prepare_listeners(std::span<const listen_config> configs) &;
    task<prepared_listener> prepare_listeners(std::span<const listen_config>) && = delete;
    [[nodiscard]] task<void> add_listener(listen_config config) &;
    task<void> add_listener(listen_config) && = delete;
    // Stages or adds a TCP/TLS service without binding a listening socket.
    // http_/https_ identify it for adopt(); HTTP/3, inherited listeners and
    // automatic redirects require listen() instead.
    [[nodiscard]] task<void> add_connection_service(listen_config config) &;
    task<void> add_connection_service(listen_config) && = delete;
    // Takes ownership even on failure. The socket must be connected and not
    // associated with another Windows IOCP; it is first bound to this loop.
    // Serves any running TCP service `service` names -- a connection service
    // or a listener -- with the same admission, PROXY header, TLS handshake,
    // budgets, timeouts and session drivers as a connection it accepted
    // itself. Owner loop, after start().
    void adopt(const listener_endpoint& service, native_socket socket, adopt_options options = {}) &;
    void adopt(const listener_endpoint&, native_socket, adopt_options = {}) && = delete;

    // Before start only. Replaces the complete trusted set atomically; invalid
    // CIDRs leave it unchanged. An empty config trusts no forwarded headers.
    http_server& trusted_proxies(trusted_proxy_config config) &;
    http_server& trusted_proxies(trusted_proxy_config) && = delete;
    // Owner loop only. Starts every staged service once; a failure closes and
    // joins all prepared resources before propagating. An empty host may start
    // and have services added later.
    [[nodiscard]] task<void> start() &;
    task<void> start() && = delete;
    // Owner loop only. Stops new admission immediately; by default established
    // sessions continue. close_connections_ closes their transports immediately;
    // session retirement is joined independently (also safe from their handlers).
    // The endpoint may then be added again.
    void remove_listener(const listener_endpoint& endpoint, listener_remove_options options = {}) &;
    void remove_listener(const listener_endpoint&, listener_remove_options = {}) && = delete;
    // Owner loop, while running. Completes once the listener `endpoint` names
    // -- serving, or the latest removed one still holding connections -- has
    // no live connection (listener_stats::active_connections_ reached zero),
    // at once when it has none. Composes "remove_listener, then wait, then
    // shutdown"; shutdown itself still closes connections directly. A
    // timeout or stop in `options` throws std::system_error (timed_out or
    // operation_canceled) and leaves the connections alone; so does the
    // host's shutdown (operation_canceled). An unknown endpoint throws
    // std::invalid_argument.
    [[nodiscard]] task<void> wait_connections_closed(const listener_endpoint& endpoint, operation_options options = {}) &;
    task<void> wait_connections_closed(const listener_endpoint&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> replace_tls_identities(const listener_endpoint& endpoint, tls_config config) &;
    task<void> replace_tls_identities(const listener_endpoint&, tls_config) && = delete;
#ifndef _WIN32
    [[nodiscard]] listener_sockets duplicate_listener_sockets(const listener_endpoint& endpoint) const&;
    listener_sockets duplicate_listener_sockets(const listener_endpoint&) const&& = delete;
#endif

    // Owner loop, while running. Requests whole-connection termination, not
    // merely a stream reset. False means no live connection has this ID.
    [[nodiscard]] bool close_connection(std::uint64_t connection_id) &;
    bool close_connection(std::uint64_t) && = delete;

    // Any thread; cancels only this host, not unrelated work on the loop.
    // Destruction requests the same close. The loop must keep running until
    // shutdown completes, or its own retirement has joined the stop callback.
    void close() & noexcept;
    void close() && = delete;
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;
    // Any thread.
    [[nodiscard]] http_server_stats stats() const;
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    std::shared_ptr<detail::http_server_state> state_;
};

// Builds one worker's callbacks; see http_server_group.
using http_server_callbacks_factory = move_only_function<http_server_callbacks(const worker_ref&)>;
// A service shared by caller-owned loops. Configuration before start does not
// require the loops to be driven. Controls suspend without blocking member
// loops and resume on the explicit caller worker (which may itself be a
// member). Drive live loops until shutdown completes. close() never stops
// caller loops; member retirement closes the group.
//
// Each member serves its connections with its own callbacks, which the
// factory builds -- for example a per-worker router that the request_stream_
// callback dispatches to. The first start() or add_connection_service() builds
// callbacks once per member, in member order, on each member's own loop
// thread before preparing any listener (never concurrently). It destroys the
// factory before that first operation completes or fails. Every member must
// set the same callback members (presence, not targets), or the operation
// throws std::invalid_argument. Shared thread-safe targets remain supported.
// A throwing factory fails that operation and closes every member; callbacks
// already built retire on their own loops after their sessions, as at shutdown.
class http_server_group final {
public:
    http_server_group(std::span<const event_loop> loops, http_server_callbacks_factory callbacks, server_config config = {});
    ~http_server_group();
    http_server_group(const http_server_group&) = delete;
    http_server_group& operator=(const http_server_group&) = delete;
    http_server_group(http_server_group&&) = delete;
    http_server_group& operator=(http_server_group&&) = delete;

    // Any thread, before start only. start() loads and checks the staged
    // configuration, dispatch_ included.
    http_server_group& listen(listen_config config) &;
    http_server_group& listen(listen_config) && = delete;
    [[nodiscard]] task<prepared_listener> prepare_listener(worker_ref caller, listen_config config) &;
    task<prepared_listener> prepare_listener(worker_ref, listen_config) && = delete;
    // 整批所有 worker 就绪后才返回；单一票据统一激活或回滚全部端点。
    [[nodiscard]] task<prepared_listener> prepare_listeners(worker_ref caller, std::span<const listen_config> configs) &;
    task<prepared_listener> prepare_listeners(worker_ref, std::span<const listen_config>) && = delete;
    [[nodiscard]] task<void> add_listener(worker_ref caller, listen_config config) &;
    task<void> add_listener(worker_ref, listen_config) && = delete;
    [[nodiscard]] task<void> add_connection_service(worker_ref caller, listen_config config) &;
    task<void> add_connection_service(worker_ref, listen_config) && = delete;
    // Before start only; replaces the complete set on every worker. Parsing
    // and allocation finish before publication; invalid input changes nothing.
    // An empty config trusts no forwarded headers.
    http_server_group& trusted_proxies(trusted_proxy_config config) &;
    http_server_group& trusted_proxies(trusted_proxy_config) && = delete;
    // Ensures every member's callbacks are built, then prepares every worker
    // before opening any listener; failure rolls back the whole host without
    // cancelling unrelated caller work.
    [[nodiscard]] task<void> start(worker_ref caller) &;
    task<void> start(worker_ref) && = delete;
    [[nodiscard]] task<void> remove_listener(worker_ref caller, const listener_endpoint& endpoint, listener_remove_options options = {}) &;
    task<void> remove_listener(worker_ref, const listener_endpoint&, listener_remove_options = {}) && = delete;
    // As http_server::wait_connections_closed(), across every member.
    [[nodiscard]] task<void> wait_connections_closed(worker_ref caller, const listener_endpoint& endpoint, operation_options options = {}) &;
    task<void> wait_connections_closed(worker_ref, const listener_endpoint&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> replace_tls_identities(worker_ref caller, const listener_endpoint& endpoint, tls_config config) &;
    task<void> replace_tls_identities(worker_ref, const listener_endpoint&, tls_config) && = delete;
#ifndef _WIN32
    [[nodiscard]] task<listener_sockets> duplicate_listener_sockets(worker_ref caller, const listener_endpoint& endpoint) const&;
    task<listener_sockets> duplicate_listener_sockets(worker_ref, const listener_endpoint&) const&& = delete;
#endif
    // Explicit target worker; takes socket ownership even when rejected.
    // As http_server::adopt() otherwise.
    [[nodiscard]] task<void> adopt(worker_ref caller, std::size_t worker_index, const listener_endpoint& service, native_socket socket, adopt_options options = {}) &;
    task<void> adopt(worker_ref, std::size_t, const listener_endpoint&, native_socket, adopt_options = {}) && = delete;
    // IDs never name another connection after retirement.
    [[nodiscard]] task<bool> close_connection(worker_ref caller, std::uint64_t connection_id) &;
    task<bool> close_connection(worker_ref, std::uint64_t) && = delete;
    void close() & noexcept;
    void close() && = delete;
    [[nodiscard]] task<void> shutdown(worker_ref caller) &;
    task<void> shutdown(worker_ref) && = delete;
    // Any thread.
    [[nodiscard]] http_server_stats stats() const;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] const worker_ref& worker(std::size_t index) const& RUVIA_LIFETIMEBOUND;
    const worker_ref& worker(std::size_t) const&& = delete;

private:
    std::shared_ptr<detail::http_server_group_state> state_;
};

}  // namespace ruvia
