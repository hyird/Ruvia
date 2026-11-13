#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_cors.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/ip_address.h"
#include "ruvia/http/proxy_protocol.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/tls_config.h"

namespace ruvia {

class static_root;

struct static_mime_type final {
    borrowed_text extension_{};
    borrowed_text content_type_{};
};

struct static_file_type_policy final {
    enum class kind : std::uint8_t {
        defaults,
        all,
        only,
    };

    kind kind_{kind::defaults};
    std::vector<std::string> extensions_{};
};

enum class static_range_request_policy : std::uint8_t {
    ignore,
    honor,
};

enum class static_response_validator_policy : std::uint8_t {
    omit,
    emit,
};

enum class static_dotfile_policy : std::uint8_t {
    deny,
    serve,
};

struct static_root_options final {
    borrowed_text cache_control_{};
    borrowed_text index_file_{"index.html"};
    borrowed_text default_content_type_{"application/octet-stream"};
    std::vector<static_mime_type> mime_types_{};
    static_file_type_policy file_types_{};
    static_range_request_policy range_requests_{static_range_request_policy::honor};
    static_response_validator_policy response_validators_{static_response_validator_policy::emit};
    // Serve files and directories whose name begins with '.' (dotfiles). Off by
    // default so a .env, .git/config, or .htpasswd sitting under the document
    // root is never exposed. Enable it only for a root that intentionally
    // publishes hidden paths (for example .well-known/ for ACME).
    static_dotfile_policy dotfiles_{static_dotfile_policy::deny};
    // Keep the bytes of files up to resident_max_bytes_, at most
    // resident_budget_bytes_ in total (in index order). Unspecified values
    // resolve to zero for a standalone static_root, or 64 KiB / 16 MiB for
    // application::document_root. An explicit zero for either disables it.
    // Resident files are served as indexed until the next snapshot, even
    // after a disk edit; a standalone root is never refreshed.
    std::optional<std::size_t> resident_max_bytes_{};
    std::optional<std::size_t> resident_budget_bytes_{};
};

namespace detail {

class static_root_access;
struct static_root_config_storage;
struct static_root_state;

}  // namespace detail

// An immutable index of the document root, built once by this constructor and
// never refreshed directly. For a configured document root the application's
// one refresher rebuilds a replacement off the workers, and each worker adopts
// it between requests.
// Relative URL keys use the generic UTF-8 form on every platform; filesystem
// I/O keeps native paths.
// Each entry records the file's size, ETag, Last-Modified and an
// identity (device, inode, modification time); serving a request looks the file
// up in that index rather than touching the directory again, so the immutable
// path costs no directory syscalls.
//
// For a standalone static_root, changing the tree does not take effect and is
// not silently tolerated either:
//
//   - A modified file fails its identity check when opened, and that request
//     errors out. Serving it from the stale index would mean sending the old
//     size for new content -- a truncated or misaligned body -- so the check
//     fails closed on purpose.
//   - A newly added file is not in the index and answers 404.
//   - A deleted file fails to open and errors out.
//   - If any filesystem operation fails while an index is being built, the
//     build fails as a whole. Refresh therefore keeps the previous complete
//     index instead of publishing a partial one.
//
// These states persist until a new static_root is constructed. application document
// roots replace it after the next successful refresh (every second by default);
// a standalone static_root does not. Resident files are served from the
// snapshot's memory until that refresh instead of failing closed on a disk
// edit. Both owners configure residency through static_root_options;
// a standalone static_root never owns refresh or compression.
class static_root final {
public:
    explicit static_root(const std::filesystem::path& root, static_root_options options = {});
    ~static_root();

    static_root(const static_root&) = delete;
    static_root& operator=(const static_root&) = delete;
    static_root(static_root&&) = delete;
    static_root& operator=(static_root&&) = delete;

    [[nodiscard]] std::filesystem::path path() const;

private:
    struct state_deleter_type final {
        void operator()(detail::static_root_state* state) const noexcept;
    };

    explicit static_root(std::unique_ptr<detail::static_root_state, state_deleter_type> state) noexcept;
    friend class detail::static_root_access;

    std::unique_ptr<detail::static_root_state, state_deleter_type> state_;
};

}  // namespace ruvia

namespace ruvia {

enum class process_signal_handler_policy : std::uint8_t {
    external_owner,
    install,
};

struct compression_config final {
    // Only text-like media types are encoded (text/*, JSON, XML and their +json
    // / +xml suffixes, JavaScript, WebAssembly, SVG, icons, uncompressed fonts);
    // a missing or other Content-Type, or one naming a zip container, stays
    // identity. A streamed response whose head declares Content-Length follows
    // the same min_bytes_ / max_bytes_ rule as a buffered body.
    // Buffered in-memory responses below this size remain identity, unless the
    // request refuses identity (e.g. "gzip, identity;q=0"): then any size is
    // encoded rather than answered with 406. File responses (context::file,
    // context::static_file) are never encoded at request time, whether their
    // bytes are read from disk or memory, so GET and HEAD describe the same
    // representation.
    std::size_t min_bytes_{1024};
    // Eligible bodies through this size are encoded synchronously on the
    // worker. It is the inline ceiling: larger bodies are offloaded to the
    // bounded blocking pool when it is enabled, and otherwise stay identity
    // (406 when the request refuses identity).
    std::size_t sync_bytes_{std::size_t{64} * 1024};
    // Buffered bodies above this size remain identity. Static files use their
    // own document-root precompression thresholds and still prefer checked-in
    // precompressed sidecars when present.
    std::size_t max_bytes_{std::size_t{64} * 1024 * 1024};
};

// The complete always-present Web server runtime configuration. application consumes
// this aggregate atomically, validates it as one unit, and publishes the same
// normalized values to every worker. Optional fields disable only the policy
// they name; optional application capabilities remain separate config-or-null calls.
// A final response that closes HTTP/1, and an HTTP/2 session that ends with
// GOAWAY, close lingering (RFC 9112 §9.6): TLS sends close_notify, TCP shuts
// down its send side, and peer input is read and discarded until the peer ends
// it, for at most 2 s and 4 MiB (fixed product limits), before TCP closes.
// Unread peer input therefore cannot reset the connection ahead of the final
// response; a lingering connection still counts as active and is closed by
// a worker stop. Idle and request-head timeout closes of a connection
// awaiting a request send TLS close_notify (RFC 9112 §9.8); write timeouts,
// transport errors and application::stop() close immediately.
// An HTTP/2 connection ends gracefully on its own (RFC 9113 §6.8) when it
// reaches max_requests_per_connection_ or a connection timeout fires: a
// first GOAWAY with a PING, then the final GOAWAY after the PING round trip
// or 1 s, whichever comes first (product limit). A timeout's drain ends the
// session at its final GOAWAY, cancelling streams still open; a write still
// blocked then is closed 2 s after the timeout. HTTP/3 does the same with its
// shutdown-notice and final GOAWAY (RFC 9114 §5.2).
// On Linux, a final buffered plain response may share its last TCP segment with
// FIN. Reusable, streamed and TLS output do not use this send hint.
// TCP may close before the session task is reclaimed. Worker shutdown still
// joins session tasks before retiring their worker-local resources.
struct server_config final {
    // Business workers, each with its own event loop; a connection stays on
    // the worker that adopted it. On POSIX every worker binds each TCP
    // listener with SO_REUSEPORT and accepts on its own event loop. On
    // Windows one acceptor thread per application owns every listener
    // exclusively (SO_EXCLUSIVEADDRUSE) and hands each accepted socket to the
    // business worker holding the fewest connections; a socket that worker's
    // queue cannot admit is closed and counted in connections_refused_.
    std::size_t worker_count_{(std::max)(1U, std::thread::hardware_concurrency())};
    process_signal_handler_policy process_signal_handlers_{process_signal_handler_policy::external_owner};
    std::size_t worker_queue_capacity_{1024};
    // HTTP connection inactivity: the keep-alive wait for the next request,
    // and a handler -- buffered or response-stream -- that neither reads nor
    // writes the connection (for a stream, the wait between two writes).
    // An HTTP/2 connection with no stream is idle from the end of its last
    // stream (or its readiness): frames that open no stream and this side's
    // writes do not renew it, as with Pingora's h2_idle_timeout; it then
    // drains with GOAWAY. Upgraded WebSockets use their route lifecycle
    // heartbeat and close-handshake deadlines instead, even when heartbeat is
    // disabled.
    std::optional<timeout_duration> idle_timeout_{std::chrono::seconds(75)};
    // How often connection timeouts are checked. A timeout fires once at least
    // its duration has passed, and at most about two intervals later. A worker
    // with no connection stops scanning until its next connection.
    timeout_duration connection_scan_interval_{std::chrono::seconds(1)};
    // Request-head inactivity; also bounds an HTTP/2 connection until the
    // client preface and SETTINGS arrive (with header_completion_timeout_).
    std::optional<timeout_duration> request_header_timeout_{std::chrono::seconds(60)};
    // Request-body inactivity. HTTP/2 times each stream's body on its own: a
    // stream that receives no DATA for this long (or whose body outlasts
    // body_completion_timeout_) is reset with RST_STREAM(CANCEL), leaving the
    // connection and its other streams untouched.
    std::optional<timeout_duration> request_body_timeout_{std::chrono::seconds(60)};
    // Absolute phase deadlines: progress does not renew these limits.
    std::optional<timeout_duration> header_completion_timeout_{std::chrono::seconds(30)};
    std::optional<timeout_duration> body_completion_timeout_{std::chrono::seconds(120)};
    // Inactivity of a connection write in progress: a buffered response, one
    // write of a response stream, a WebSocket or CONNECT tunnel write, or
    // HTTP/2 frame output (its RFC 8441 WebSockets and tunnels included).
    // Only write progress renews this limit -- input does not -- and it is not
    // a deadline for completing the whole response. It applies whatever the
    // read side is doing (idle, reading a body, a long-lived upgrade) and
    // whether or not a WebSocket heartbeat is configured: a peer that stops
    // reading has its connection closed. Time between response-stream writes
    // is governed by idle_timeout_. An HTTP/2 stream that waits this long for
    // peer flow-control window (send window 0) is reset with
    // RST_STREAM(CANCEL); its connection and other streams continue. A file
    // body read the next write waits on (on the blocking pool) is bounded by
    // the same limit: when it expires the response is answered with 503
    // before its head, or its connection (HTTP/1) or stream (HTTP/2) ends.
    // So is the open context::file() and context::static_file() wait on: its
    // task then throws a 503 file_read_unavailable http_error.
    std::optional<timeout_duration> write_timeout_{std::chrono::seconds(60)};
    // Bounded by default so a connection flood cannot exhaust FDs or memory.
    // At the cap, a new connection takes the slot of the connection that has
    // been idle between requests longest -- an HTTP/1 keep-alive connection
    // awaiting its next request, or an HTTP/2 connection without a stream --
    // whose socket is closed at once (as nginx reuses keep-alive connections
    // when worker_connections run out); active_connections_ may exceed the cap
    // by such closes until their sessions unwind. With no idle connection the
    // new socket is closed before TLS or HTTP protocol detection, so admission
    // never fabricates a response, and counted in connections_refused_.
    std::optional<std::size_t> max_connections_per_worker_{1024};
    // Cumulative request cap for each connection. HTTP/2 then drains gracefully
    // (RFC 9113 section 6.8): streams the client had in flight before the final
    // GOAWAY are still served, at most 128 beyond the cap. HTTP/3 sends its
    // shutdown-notice GOAWAY and the final one alike (RFC 9114 §5.2).
    std::optional<std::size_t> max_requests_per_connection_{1000};
    std::size_t max_buffered_body_bytes_{default_max_buffered_body_bytes};
    // Live inbound buffer allocations across buffered and streamed HTTP request
    // bodies, CONNECT tunnel reads (including chunks a handler still holds) and
    // websocket sessions. Includes capacity and reallocation overlap; the
    // limits cannot be disabled. An allocation past either limit fails with
    // std::bad_alloc, which an unhandled request answers with 503.
    std::size_t max_inbound_buffer_bytes_per_worker_{256 * 1024 * 1024};
    std::size_t max_inbound_buffer_bytes_per_connection_{64 * 1024 * 1024};
    std::optional<std::size_t> max_stream_body_bytes_{};
    std::size_t max_websocket_message_bytes_{default_max_websocket_message_bytes};
    // Shared by all registered outbound HTTP client aliases on each worker.
    http_client_worker_config http_client_worker_{};
    // Host-wide representation policies applied to every HTTP response this
    // host prepares: routes, router::dispatch() and request_stream callbacks
    // alike. Absence disables the policy. compression_ also enables
    // negotiation of indexed precompressed static variants; CORS headers
    // also follow the route engine's OPTIONS preflight answer.
    std::optional<compression_config> compression_{};
    std::optional<http_cors_policy_config> cors_{};
};

struct trusted_proxy_config final {
    // CIDR lengths count bits in the literal's address family. For example,
    // 127.0.0.0/8 and ::ffff:127.0.0.0/104 describe the same mapped network.
    std::vector<std::string> cidrs_{};
    // Priority-ordered forwarding headers. When empty, defaults to
    // {"X-Forwarded-For", "Forwarded"}.
    std::vector<http_forwarded_header_spec> forwarding_headers_{};
    // Enable only when every trusted proxy sanitizes and appends matching
    // X-Forwarded-For / X-Forwarded-Proto elements.
    bool trust_x_forwarded_proto_{false};
};

// A socket handle as the platform names it: a file descriptor on POSIX, a
// SOCKET on Windows.
#ifdef _WIN32
using native_socket = std::uintptr_t;
#else
using native_socket = int;
#endif

// Already-bound sockets of one listen_config's services: listening TCP
// sockets for http_ and https_, bound UDP sockets for http3_. On POSIX a
// non-empty vector holds one socket per worker in worker order, the members
// of one SO_REUSEPORT group; on Windows it holds the one socket the acceptor
// thread owns. See listen_config::inherited_sockets_ and
// application::duplicate_listener_sockets().
// When http_ and https_ name the same port there is one TCP socket group,
// held only in https_; http_ must be empty for inheritance.
struct listener_sockets final {
    std::vector<native_socket> http_{};
    std::vector<native_socket> https_{};
    std::vector<native_socket> http3_{};
};

namespace detail {
struct access_log_record_access;
struct connection_failure_record_access;
}  // namespace detail

// What a listener serves on the connections it accepts.
enum class listener_dispatch : std::uint8_t {
    // HTTP through the application's routes, middleware and controllers.
    // Only application has routes; http_server and http_server_group reject
    // it (their request_stream callback may await router::dispatch()).
    routes,
    // The host's request_stream callback (application::on_request_stream or
    // http_server_callbacks::request_stream_) receives each HTTP request --
    // HTTP/1.1, HTTP/2 and HTTP/3 alike -- as a request_stream.
    request_stream,
    // The host's connection callback (application::on_connection or
    // http_server_callbacks::connection_) receives each accepted connection
    // -- on a TLS listener once its handshake completed -- as a byte stream
    // (server_connection); the server parses no HTTP on it.
    connection,
};

// One bind address with optional HTTP, HTTPS and HTTP/3 service ports,
// independent of the wire protocol. application validates and expands this
// value atomically. An omitted port disables that service. auto_https_redirect_
// targets the HTTPS service port. On POSIX every worker binds each enabled
// service with SO_REUSEADDR and SO_REUSEPORT (platforms without SO_REUSEPORT
// fail startup); on Windows the acceptor thread binds it exclusively
// (SO_EXCLUSIVEADDRUSE, see server_config::worker_count_). inherited_sockets_
// replaces either binding with sockets bound elsewhere.
struct listen_config final {
    // Numeric IPv4 or IPv6 bind address, normalized when application::listen consumes
    // this configuration.
    borrowed_text address_{"0.0.0.0"};
    // 未设置时保留操作系统默认；显式值仅接受 IPv6 地址，绑定前同时
    // 应用于 TCP 与 QUIC UDP。继承 socket 必须已经具有相同的值。
    std::optional<bool> ipv6_only_{};
    // Values are checked before narrowing; an enabled service must be non-zero.
    std::optional<port_number> http_{};
    std::optional<port_number> https_{};
    // Equal HTTP and HTTPS ports share one physical TCP listener. Before
    // consuming input, first byte 0x16 selects TLS; every other byte selects
    // plaintext, including for connection dispatch. Detection shares the TLS
    // handshake deadlines; TLS failures never retry as plaintext. Distinct
    // ports keep their strict plaintext / TLS behavior.
    // HTTP/3 (RFC 9114) over QUIC on this UDP port, serving the HTTPS
    // listener's tls_ identities with TLS 1.3 and ALPN "h3" only. Requires
    // https_ and a build with RUVIA_ENABLE_HTTP3; it may equal the https_
    // number, since UDP and TCP ports are distinct. Every response of the
    // HTTPS listener (HTTP/1.1 and HTTP/2) then advertises it with
    // `Alt-Svc: h3=":<port>"; ma=<http3_alt_svc_max_age_>` (RFC 7838 §3)
    // unless the application set its own Alt-Svc field. POSIX workers each
    // bind the UDP port with SO_REUSEPORT and steer packets of a connection
    // to the worker that owns it by its connection ID. On Windows the
    // acceptor thread owns the UDP socket exclusively: it steers each packet
    // to the worker its connection ID names, a new connection's Initial to
    // the worker a keyed hash of the client's chosen ID selects (so its
    // retransmissions reach the same worker), and sends the datagrams workers
    // hand back through a bounded queue -- one past the bound is lost, as UDP
    // may lose any. Active client connection migration and NAT rebinding are
    // supported, 0-RTT is opt-in via http3_early_data_, stateless Retry address
    // validation is opt-in via http3_retry_, Stateless Reset is sent for
    // retired server connection IDs, and CONNECT, RFC 9220 WebSocket and
    // RFC 9297/9298 CONNECT-UDP / HTTP Datagrams are supported. The server
    // limits apply per request stream as for HTTP/2: idle_timeout_ is also
    // the QUIC max_idle_timeout, request_header_timeout_ and
    // header_completion_timeout_ bound the handshake, and a request body or an
    // output wait (the peer not acknowledging) that outlasts its timeout
    // resets only that stream with H3_REQUEST_CANCELLED. A request cap or
    // connection timeout sends the shutdown-notice GOAWAY, the final GOAWAY
    // after min(1 s, three probe timeouts), then CONNECTION_CLOSE with
    // H3_NO_ERROR once every admitted stream closed (RFC 9114 §5.2);
    // application::stop() sends CONNECTION_CLOSE(H3_NO_ERROR) at once.
    std::optional<port_number> http3_{};
    // Alt-Svc freshness lifetime (RFC 7838 §3 "ma"); must be positive.
    std::chrono::seconds http3_alt_svc_max_age_{std::chrono::hours(24)};
    // Enable replayable QUIC 0-RTT; unsafe methods are rejected with 425.
    bool http3_early_data_{false};
    // Require stateless QUIC Retry address validation before opening a session.
    bool http3_retry_{false};
    tls_config tls_{};
    bool auto_https_redirect_{false};
    // On a shared HTTP/HTTPS port only plaintext connections redirect; TLS
    // connections serve normally, including HTTP/2 and any paired HTTP/3.
    // Applies to the http_, https_ and http3_ services alike; with
    // auto_https_redirect_ the http_ service only redirects.
    // listener_dispatch::connection rejects http3_ and auto_https_redirect_
    // (std::invalid_argument), since neither serves a byte stream.
    // Unset: application serves routes; http_server and http_server_group
    // derive it from their callbacks -- request_stream when only
    // request_stream_ is set, connection when only connection_ is set -- and
    // reject (std::invalid_argument) a listener whose host sets both or
    // neither. An explicit value whose callback is absent is rejected too.
    // Standalone hosts resolve and check it when the listener is loaded:
    // start() for listen(), the call itself for add_listener(),
    // prepare_listener(s)() and add_connection_service().
    std::optional<listener_dispatch> dispatch_{};
    // Every connection of the TCP services must begin with a PROXY protocol
    // header (HAProxy PROXY protocol v1 or v2); nothing is guessed, and a
    // connection without a valid header is closed before TLS or HTTP. The
    // Header limit: 4096 bytes (v1: 107 bytes). The absolute deadline is the
    // minimum configured request_header_timeout_ / header_completion_timeout_;
    // when both are disabled it is 30 s. A PROXY command's
    // source becomes the connection's remote endpoint for every consumer --
    // admission, open/close events, conn_info, access logs, rate limiting and
    // the trusted-proxy check; a LOCAL command, or an address family without
    // IP endpoints, keeps the real peer. The local endpoint stays the
    // socket's. Rejected (std::invalid_argument) with http3_, which has no
    // byte stream to carry it.
    bool proxy_protocol_{false};
    // Sockets that replace binding the services they name, for example ones
    // another process exported with application::duplicate_listener_sockets()
    // and passed across exec. Each must already serve its service on address_
    // and the configured port -- http_ and https_ sockets listening TCP,
    // http3_ sockets bound UDP; a wildcard address_ matches a wildcard bind --
    // and a non-empty vector requires its service. listen() and
    // application::add_listener() own every handle from the call on and mark
    // it close-on-exec (non-inheritable on Windows); a handle given twice or
    // failing a check throws std::invalid_argument, and any failure of that
    // call closes every handle it was given. An empty vector binds as usual.
    // The next run() consumes them, whatever its outcome, and later runs
    // bind; handles listen() replaces unconsumed close. On POSIX a vector
    // holds one socket per worker, else run() (or add_listener()) throws
    // std::invalid_argument; socket i serves worker i, and for http3_ it must
    // be the i-th member of its SO_REUSEPORT group, as
    // duplicate_listener_sockets() returns them, or QUIC packets reach their
    // connection's worker only through the user-space hand-over. On Windows a
    // vector holds at most one socket (std::invalid_argument otherwise), which
    // the acceptor thread uses as it would a socket it bound; a socket another
    // process associated with an I/O completion port cannot be adopted, which
    // fails startup.
    // A shared HTTP/HTTPS port accepts TCP handles only in https_; a non-empty
    // http_ vector is rejected, and all supplied handles close on failure.
    std::optional<listener_sockets> inherited_sockets_{};
};

// Names one TCP service a listen_config enabled: its address_, normalized
// the same way, and its http_ or https_ port. A paired HTTP/3 service is
// reached through its HTTPS endpoint.
struct listener_endpoint final {
    borrowed_text address_{"0.0.0.0"};
    port_number port_{};
};

struct listener_remove_options final {
    // 按稳定监听身份立即关闭所有既有连接（包括 TLS、多路复用与升级）。
    // 默认只停止新接入；相同端点后来新增的监听不受影响。
    bool close_connections_{false};
};

// How adopt() takes over a connection something else accepted, and possibly
// already read from.
struct adopt_options final {
    // Bytes already read from the socket, in order, before any byte still in
    // it. The service consumes them first, exactly as if read from the socket:
    // a PROXY header on a proxy_protocol_ service, TLS detection and the
    // ClientHello, or HTTP/1, HTTP/2 or connection-callback input. Copied
    // before adopt() returns (before its task starts, for the group).
    std::span<const std::byte> prefix_{};
    // Replaces the socket's peer as the connection's remote endpoint, for
    // example after the caller parsed a PROXY header itself. A PROXY header
    // the service itself parses still takes precedence.
    std::optional<ip_endpoint> remote_{};
};

// Runtime behavior belongs to the server's document-root binding, not to the
// immutable static_root index. application document roots are always refreshed:
// one refresher per application rebuilds the index each interval and every
// worker adopts the shared snapshot. A standalone static_root remains immutable
// because it has no server runtime.
struct document_root_config final {
    std::filesystem::path root_{};
    static_root_options static_options_{};
    std::chrono::milliseconds refresh_interval_{std::chrono::seconds(1)};
    bool precompress_gzip_{false};
    bool precompress_brotli_{false};
    bool precompress_zstd_{false};
    std::size_t precompress_min_bytes_{1024};
    std::size_t precompress_max_bytes_{std::size_t{256} * 1024};
};

// One terminal response outcome with a committed final status, passed to the
// access-log callback after a complete buffered response head has reached the
// transport, a stream head is committed, or a CONNECT/WebSocket handshake head
// has reached the transport (before the long-lived handler runs). The record
// and all returned views are valid only for the callback.
class access_log_record final {
public:
    [[nodiscard]] constexpr std::string_view method() const noexcept {
        return method_;
    }

    [[nodiscard]] constexpr http_known_method known_method() const noexcept {
        return known_method_;
    }

    [[nodiscard]] constexpr std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] constexpr ip_address client_address() const noexcept {
        return client_address_;
    }

    [[nodiscard]] constexpr ip_address remote_address() const noexcept {
        return remote_address_;
    }

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr std::uint64_t duration_micros() const noexcept {
        return duration_micros_;
    }

    [[nodiscard]] constexpr std::uint64_t db_count() const noexcept {
        return db_count_;
    }

    [[nodiscard]] constexpr std::uint64_t db_duration_micros() const noexcept {
        return db_duration_micros_;
    }

    [[nodiscard]] constexpr std::uint64_t redis_count() const noexcept {
        return redis_count_;
    }

    [[nodiscard]] constexpr std::uint64_t redis_duration_micros() const noexcept {
        return redis_duration_micros_;
    }
    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

private:
    friend struct detail::access_log_record_access;

    struct fields_type final {
        std::string_view method_;
        http_known_method known_method_;
        std::string_view path_;
        http_protocol_version protocol_version_;
        ip_address remote_address_{};
        ip_address client_address_{};
        http_status_code status_;
        std::uint64_t duration_micros_;
        std::uint64_t db_count_{0};
        std::uint64_t db_duration_micros_{0};
        std::uint64_t redis_count_{0};
        std::uint64_t redis_duration_micros_{0};
    };

    explicit constexpr access_log_record(const fields_type& fields) noexcept
        : method_(fields.method_),
          path_(fields.path_),
          remote_address_(fields.remote_address_),
          client_address_(fields.client_address_),
          duration_micros_(fields.duration_micros_),
          db_count_(fields.db_count_),
          db_duration_micros_(fields.db_duration_micros_),
          redis_count_(fields.redis_count_),
          redis_duration_micros_(fields.redis_duration_micros_),
          status_(fields.status_),
          known_method_(fields.known_method_),
          protocol_version_(fields.protocol_version_) {}

    std::string_view method_;
    std::string_view path_;
    ip_address remote_address_{};
    ip_address client_address_{};
    std::uint64_t duration_micros_;
    std::uint64_t db_count_{0};
    std::uint64_t db_duration_micros_{0};
    std::uint64_t redis_count_{0};
    std::uint64_t redis_duration_micros_{0};
    http_status_code status_;
    http_known_method known_method_;
    http_protocol_version protocol_version_;
};

namespace detail {
using access_log_callback_ref = callback_ref<void(const access_log_record&) noexcept>;
}  // namespace detail

// application-owned access-log listener. Request dispatch receives only an internal,
// allocation-free callback_ref and never participates in this owner's lifetime.
// 空函数指针构造空回调；未配置时不得调用。所有权与退役语义同 move_only_function。
using access_log_callback = move_only_function<void(const access_log_record&) noexcept>;

// One connection lost to an exception that escaped its session: a handler bug
// past the response's point of no return, an error handler that itself failed,
// or resource exhaustion. The server closes that connection and keeps serving;
// this record is how the failure becomes visible instead of vanishing with the
// connection. A request that fails before its response is committed is
// answered with a 5xx through on_error and never gets here, with one
// exception: a failed write of an interim head or 100 Continue on the
// connection's own transport ends the connection and is reported here unless
// it is a peer reset/close or server-cancelled I/O.
//
// Views and the record itself are valid only for the callback invocation.
class connection_failure_record final {
public:
    // Empty when the peer address could not be read (the connection was
    // already gone) or when the failure happened before it was resolved.
    [[nodiscard]] constexpr ip_address remote_address() const noexcept {
        return remote_address_;
    }

    // Never null. Rethrow it to inspect the failure.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

private:
    friend struct detail::connection_failure_record_access;

    connection_failure_record(ip_address remote_address, std::exception_ptr exception) noexcept
        : remote_address_(remote_address),
          exception_(std::move(exception)) {}

    ip_address remote_address_{};
    std::exception_ptr exception_;
};

// Live connections of one serving or removed-but-not-retired listener, named
// as its listener_endpoint: the normalized address_ text and its http_ or
// https_ port. A paired HTTP/3 service's QUIC connections count under its
// HTTPS endpoint.
struct listener_stats final {
    ip_address address_{};
    port_number port_{};
    std::size_t active_connections_{0};
};

// HTTP serving counters, for health checks and metrics. Cumulative since the
// worker started and never reset, except active_connections_ and listeners_,
// which are gauges. Every host's stats() sums these across its workers; each
// field is sampled independently, so treat them as a set of gauges rather
// than one snapshot.
//
// These make a server observable without installing any callback: on_error sees
// request failures and on_connection_failure sees lost connections, but neither
// answers "how many, since when".
struct http_server_stats final {
    // One entry per listener in listener order, with the connections it
    // admitted that are still live. A removed listener stays listed until
    // its last connection ended.
    std::vector<listener_stats> listeners_{};
    // Connections held right now, against server_config::max_connections_per_worker_.
    std::size_t active_connections_{0};
    // Connections closed on accept because that budget was full and no idle
    // connection could give up its slot (an idle one evicted to admit a new
    // connection is not counted). A rising count means the server is shedding
    // load rather than queueing it.
    std::size_t connections_refused_{0};
    // Connections lost to an exception, as delivered to on_connection_failure
    // (peer resets and closes and server-cancelled I/O are not failures).
    std::size_t connection_failures_{0};
    // Accept attempts or session starts that failed, excluding peer disconnects
    // and our own cancellation. Fatal listener failures also stop the worker.
    std::size_t accept_failures_{0};
    // Failures that escaped to the worker's io_context and stopped it.
    std::size_t worker_failures_{0};
    // Document-root refresh passes whose replacement index could not be built
    // or adopted. The application runs one refresher however many workers it
    // has, so each failed pass counts once. The previous complete snapshot
    // remains active; this counter makes filesystem/permission failures
    // observable without taking a worker down, and each failure's cause goes
    // to the rate-limited unhandled-failure report (stderr).
    std::size_t document_root_refresh_failures_{0};
};

namespace detail {
using connection_failure_callback_ref = callback_ref<void(const connection_failure_record&) noexcept>;
}  // namespace detail

// application-owned connection-failure listener. The listener must not throw: it runs on the last
// line of defense for a connection, where a second failure would have nowhere
// left to go, so the requirement is enforced at compile time rather than
// swallowed at runtime.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using connection_failure_callback = move_only_function<void(const connection_failure_record&) noexcept>;

// A self-contained startup/shutdown callback, invoked on the application::run() caller.
// on_start runs after worker initialization, before TCP admission. on_stop
// runs after admission closes, before runtime join (including start-hook failure
// or cancellation). A self-contained callable is owned by the hook value;
// references captured by that callable must still
// outlive the registered hook. Invoking an empty hook is a programming error
// and throws std::logic_error.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using app_hook = move_only_function<void()>;
}  // namespace ruvia

namespace ruvia {
namespace detail {
class prepared_listener_state;
struct prepared_listener_access;
}  // namespace detail

// 拥有一批已绑定但尚未接入的监听事务。TCP 握手可能完成，但处理器不会执行。
// prepare 成功时 TLS、QUIC 与全部 worker 资源已就绪；同宿主的其他监听
// 控制等待该事务结束。多个端点须用 prepare_listeners 同批准备，不能在持有
// 票据时再 await 本宿主监听控制。先发布业务配置，再 activate()。
// 任一项准备失败、放弃票据或宿主关闭均回滚整批，不影响原有监听。
// 可跨线程移动、析构；activate/cancel 必须在 prepare 的 caller worker 上 await。
// 消耗型操作在调用时接管句柄；丢弃冷任务或析构句柄均请求回滚。
// 析构不阻塞；cancel() 等待回滚完成。caller 与宿主循环须驱动至操作完成。
class prepared_listener final {
public:
    prepared_listener(prepared_listener&&) noexcept;
    prepared_listener& operator=(prepared_listener&&) noexcept;
    prepared_listener(const prepared_listener&) = delete;
    prepared_listener& operator=(const prepared_listener&) = delete;
    ~prepared_listener();

    // 只提交已完成的准备；已关闭宿主或空句柄抛 logic_error。
    [[nodiscard]] task<void> activate() &&;
    [[nodiscard]] task<void> cancel() &&;
    [[nodiscard]] bool valid() const noexcept;

private:
    explicit prepared_listener(std::shared_ptr<detail::prepared_listener_state>) noexcept;
    std::shared_ptr<detail::prepared_listener_state> state_;
    friend struct detail::prepared_listener_access;
};

}  // namespace ruvia

namespace ruvia {

class connection_event final {
public:
    connection_event(std::uint64_t id, ip_endpoint local, ip_endpoint remote, bool quic) noexcept
        : id_(id),
          local_(std::move(local)),
          remote_(std::move(remote)),
          quic_(quic) {}
    [[nodiscard]] std::uint64_t connection_id() const noexcept {
        return id_;
    }
    [[nodiscard]] const ip_endpoint& local() const& noexcept RUVIA_LIFETIMEBOUND {
        return local_;
    }
    const ip_endpoint& local() const&& = delete;
    [[nodiscard]] const ip_endpoint& remote() const& noexcept RUVIA_LIFETIMEBOUND {
        return remote_;
    }
    const ip_endpoint& remote() const&& = delete;
    [[nodiscard]] bool quic() const noexcept {
        return quic_;
    }

private:
    std::uint64_t id_;
    ip_endpoint local_;
    ip_endpoint remote_;
    bool quic_;
};

enum class connection_close_reason : std::uint8_t {
    normal,
    peer_eof,
    timeout,
    cancelled,
    aborted,
    protocol_error,
    transport_error,
    handler_error,
};

struct connection_close_info final {
    connection_close_reason reason_{connection_close_reason::normal};
    // Transport bytes actually received/sent, including TLS ciphertext and
    // QUIC retransmissions; replayed prefixes never count twice.
    std::uint64_t bytes_in_{};
    std::uint64_t bytes_out_{};
    // New HTTP request heads, once per request; raw connections remain zero.
    std::uint64_t requests_{};
};

namespace detail {
using connection_admission_callback_ref = callback_ref<bool(const connection_event&) noexcept>;
using connection_open_callback_ref = callback_ref<std::shared_ptr<void>(const connection_event&)>;
using connection_close_callback_ref = callback_ref<void(const connection_event&, const std::shared_ptr<void>&, const connection_close_info&) noexcept>;
}  // namespace detail

// Synchronous worker callbacks; targets must support concurrent workers. False
// admission silently rejects before creating TLS or protocol session resources.
// Rejected connections have no open/close events. Every admitted connection has
// one open and one close, including failed handshakes and cancelled cold tasks.
// The optional shared state returned by open is retained through close. Contexts
// borrow its connection slot; connection_state() copies its shared ownership.
// The close argument borrows that shared_ptr only for the callback; copy it to
// retain the object beyond connection retirement.
// Close runs after ID retirement, outside locks, before the server releases its
// state reference. Admission and close must not throw. If open throws, close
// still runs once with empty state, and the failure reaches the failure sink.
using connection_admission_callback = move_only_function<bool(const connection_event&) noexcept>;
using connection_open_callback = move_only_function<std::shared_ptr<void>(const connection_event&)>;
using connection_close_callback = move_only_function<void(const connection_event&, const std::shared_ptr<void>&, const connection_close_info&) noexcept>;

}  // namespace ruvia
