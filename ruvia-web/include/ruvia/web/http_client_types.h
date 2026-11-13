#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request_content_semantics.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/ip_address.h"
#include "ruvia/web/tls_config.h"

namespace ruvia {

enum class tcp_no_delay_policy : std::uint8_t {
    system_default,
    enable,
};

enum class tcp_keep_alive_policy : std::uint8_t {
    system_default,
    enable,
};

// Socket policies applied to every outbound TCP connection of one client.
// Establishment fails when an enabled policy cannot be applied.
struct client_tcp_config final {
    tcp_no_delay_policy no_delay_{tcp_no_delay_policy::enable};
    tcp_keep_alive_policy keep_alive_{tcp_keep_alive_policy::enable};
};

}  // namespace ruvia

namespace ruvia {

// The HTTP versions a request may use: at least one, each built into this
// library (std::invalid_argument otherwise).
// https: TCP connections offer TLS ALPN for http1_ and http2_. http3_ alone
// dials QUIC directly; beside a TCP version it follows an Alt-Svc h3 that an
// authenticated response of the same route advertised, and without http3_
// Alt-Svc never upgrades a request.
// http: http1_ uses HTTP/1.1, http2_ alone prior-knowledge h2c; http3_ needs
// https.
struct http_client_versions final {
    bool http1_{true};
#ifdef RUVIA_ENABLE_HTTP2
    bool http2_{true};
#else
    bool http2_{false};
#endif
    bool http3_{false};

    friend constexpr bool operator==(const http_client_versions&, const http_client_versions&) noexcept = default;
};

enum class http_client_received_cookie_policy : std::uint8_t {
    ignore,
    retain_and_send,
};

// 默认继承；disabled() 只禁用这一阶段，不修改总截止期或宿主取消。
class http_client_timeout_override final {
public:
    http_client_timeout_override() noexcept = default;
    [[nodiscard]] static http_client_timeout_override disabled() noexcept {
        return http_client_timeout_override(std::nullopt);
    }
    [[nodiscard]] static http_client_timeout_override after(timeout_duration duration) {
        if (duration.value().count() <= 0) {
            throw std::invalid_argument("HTTP phase timeout must be positive");
        }
        return http_client_timeout_override(duration);
    }
    [[nodiscard]] std::optional<timeout_duration> resolve(std::optional<timeout_duration> inherited) const noexcept {
        return overridden_ ? duration_ : inherited;
    }

private:
    explicit http_client_timeout_override(std::optional<timeout_duration> duration) noexcept
        : duration_(duration),
          overridden_(true) {}
    std::optional<timeout_duration> duration_;
    bool overridden_{false};
};

struct http_client_phase_timeouts final {
    http_client_timeout_override connect_;
    http_client_timeout_override read_;
    http_client_timeout_override write_;
    http_client_timeout_override acquire_;
};

// 按线序 await；字段借用只持续到回调完成。回调应响应 stop，不得等待本次
// exchange 或依赖其释放池容量的操作；异常使该 exchange 失败。HTTP/2/3 只背压
// 本响应，不阻塞其他流；总截止期不因回调挂起而暂停。重定向的每一跳共用同一回调。
using http_client_informational_callback = move_only_function<task<void>(http_status_code, std::span<const http_header>, stop_token)>;

// Validates each numeric candidate address (ip_address, port) before it is dialed.
// Every worker serving the alias calls the same shared target, possibly concurrently.
using http_client_address_validator = move_only_function<bool(const ip_address&, std::uint16_t)>;
// Move-only: it owns the request's informational callback.
struct http_client_request_options final {
    // nullopt 继承 origin 默认；false 为保留编码字段的原样流。
    std::optional<bool> decode_content_;
    http_request_field_source field_source_{http_request_field_source::generated};
    // 独立物理连接，既不借业务连接，也不归还业务复用池。
    bool fresh_connection_{false};
    http_client_phase_timeouts timeouts_;
    http_client_informational_callback informational_;
    // 请求级路由覆盖；未设字段继承 alias，scheme 始终来自 alias 的 base_url_。
    // authority_：逻辑 Host / :authority（host[:port]，无 userinfo）。不同于
    // alias authority 时 Cookie jar 不参与，跨 origin 重定向丢弃全部覆盖；
    // Alt-Svc 按实际路由（拨号 host、拨号端口、server name）记录与使用。
    std::optional<std::string> authority_;
    // 物理拨号目标；域名只在新建连接时解析，并经 address_validator_ 校验。
    std::optional<std::string> dial_host_;
    std::optional<port_number> dial_port_;
    // TLS SNI 与证书校验名，默认取有效 authority 的 host（IP 字面量不发 SNI，
    // 按 IP SAN 校验）。连接按 (拨号 host, 拨号端口, server name, ALPN) 复用，
    // Host / :authority 不入键：HTTP/2/3 连接上发送其他 authority 时，调用方
    // 负责该证书覆盖它（RFC 9113 §9.1.1、RFC 9114 §3.3）。
    std::optional<std::string> server_name_;
    // Replaces the alias's versions_ for this request; part of its connection key.
    std::optional<http_client_versions> versions_;
};

enum class http_client_response_framing : std::uint8_t {
    none,
    fixed_length,
    chunked,
    close_delimited,
    stream,
    tunnel,
    protocol_upgrade
};

struct http_client_connection_info final {
    // 物理连接身份，不是槽位。重新拨号产生新 id，旧 id 不能关闭新连接。
    std::uint64_t id_{};
    ip_address peer_ip_{};
    std::uint16_t peer_port_{};
    bool reused_{false};
    std::chrono::steady_clock::duration connect_duration_{};
    std::chrono::steady_clock::duration tls_duration_{};
};

// Per-hop redirect observation passed to on_redirect / redirect_policy before
// following a redirect response. Views are valid for the duration of the callback.
class http_client_redirect_hop final {
public:
    http_client_redirect_hop(std::size_t hop_index, http_origin_view source_origin,
        std::string_view source_target, std::string_view source_url,
        const http_client_connection_info& connection_info, http_status_code status,
        std::string_view location, http_origin_view target_origin,
        std::string_view target, std::string_view target_url, bool cross_origin) noexcept
        : hop_index_(hop_index),
          source_origin_(source_origin),
          source_target_(source_target),
          source_url_(source_url),
          connection_info_(&connection_info),
          status_(status),
          location_(location),
          target_origin_(target_origin),
          target_(target),
          target_url_(target_url),
          cross_origin_(cross_origin) {}

    [[nodiscard]] constexpr std::size_t hop_index() const noexcept {
        return hop_index_;
    }
    [[nodiscard]] constexpr http_origin_view source_origin() const noexcept {
        return source_origin_;
    }
    [[nodiscard]] constexpr std::string_view source_target() const noexcept {
        return source_target_;
    }
    [[nodiscard]] constexpr std::string_view source_url() const noexcept {
        return source_url_;
    }
    [[nodiscard]] const http_client_connection_info& connection_info() const noexcept {
        return *connection_info_;
    }
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }
    [[nodiscard]] constexpr std::string_view location() const noexcept {
        return location_;
    }
    [[nodiscard]] constexpr http_origin_view target_origin() const noexcept {
        return target_origin_;
    }
    [[nodiscard]] constexpr std::string_view target() const noexcept {
        return target_;
    }
    [[nodiscard]] constexpr std::string_view target_url() const noexcept {
        return target_url_;
    }
    [[nodiscard]] constexpr bool cross_origin() const noexcept {
        return cross_origin_;
    }

private:
    std::size_t hop_index_{0};
    http_origin_view source_origin_;
    std::string_view source_target_{};
    std::string_view source_url_{};
    const http_client_connection_info* connection_info_{nullptr};
    http_status_code status_;
    std::string_view location_{};
    http_origin_view target_origin_;
    std::string_view target_{};
    std::string_view target_url_{};
    bool cross_origin_{false};
};

using http_client_redirect_observer = move_only_function<bool(const http_client_redirect_hop&)>;
using http_client_async_redirect_observer =
    move_only_function<task<bool>(const http_client_redirect_hop&, stop_token)>;

struct http_client_endpoint_address final {
    http_client_endpoint_address() noexcept = default;
    constexpr http_client_endpoint_address(ip_address ip, port_number port) noexcept
        : ip_(ip),
          port_(port) {}
    http_client_endpoint_address(std::string_view ip, port_number port)
        : ip_(parse_ip(ip)),
          port_(port) {}

    ip_address ip_{};
    port_number port_{};

private:
    [[nodiscard]] static ip_address parse_ip(std::string_view text) {
        const auto parsed = ip_address::parse(text);
        if (!parsed.has_value()) {
            throw std::invalid_argument("HTTP client endpoint IP must be a valid IP literal");
        }
        return *parsed;
    }
};

// Connection advertisements are optional observations. They do not change the
// fixed registered origin, connection coalescing, TLS identity or cache policy.
struct http_client_advertisement_config final {
    bool receive_origins_{false};
    bool receive_alternative_services_{false};
    std::size_t max_queued_advertisements_{32};
    // Includes observations retained by callers after polling the client.
    std::size_t max_retained_bytes_{64 * 1024};
};

struct http_client_push_config final {
    bool enabled_{false};
    std::size_t max_queued_pushes_{16};
    std::size_t max_concurrent_pushes_{16};
    std::optional<timeout_duration> timeout_{30000};
};

struct http_client_upload_config final {
    // A known length must be fulfilled exactly; HTTP/1 trailers require chunked
    // framing. Without trailers, HTTP/1 finalization uses fixed-size wire storage.
    std::optional<http_content_length> content_length_{};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    std::size_t max_chunk_bytes_{64 * 1024};
    timeout_duration continue_timeout_{1000};
};

// connect(...).open() and udp(...).open() select the protocol; this policy bounds
// the size of one transport write, independently of Capsule payload limits.
struct http_client_tunnel_config final {
    std::size_t max_chunk_bytes_{64 * 1024};
    // Once accepted, the tunnel closes with timeout after this long without
    // bytes in either direction; read_timeout_ no longer applies. nullopt
    // leaves it unbounded except by request_timeout_.
    std::optional<timeout_duration> idle_timeout_{};
};
// Bounds bytes retained by response bytes()/text() and stream collection: per standalone client or,
// in an application, across all registered aliases of one business worker. This is
// independent of per-response limits and worker-owned memory.
struct http_client_result_budget_config final {
    std::size_t max_retained_bytes_{std::size_t{64} * 1024 * 1024};
    // Worker-local live response allocations, including compressed input,
    // decoder output, metadata and simultaneous buffer growth.
    std::size_t max_in_flight_bytes_{std::size_t{64} * 1024 * 1024};
};

// Shared by every alias of one standalone http_client, or of one application
// worker. Alias connection_limits_ still apply: the tighter bound wins.
struct http_client_worker_config final {
    http_client_result_budget_config result_budget_{};
    // Idle connections kept across all aliases; beyond these the least
    // recently used idle connection of that kind, of any alias, closes.
    std::size_t max_idle_http1_{256};
    // HTTP/2 and HTTP/3 connections without streams.
    std::size_t max_idle_multiplexed_{256};
    // Background DNS refresh lookups (dns_refresh_interval_) running at once;
    // a refresh period finding none free defers to the next period.
    std::size_t max_concurrent_dns_refreshes_{8};
};

struct http_client_http3_config final {
    std::size_t max_concurrent_streams_{100};
    std::size_t max_alt_svc_entries_{16};
    bool enable_early_data_{false};
    std::size_t max_datagram_bytes_{1350};
    std::size_t max_stream_output_bytes_{256 * 1024};
};

// Connections are created on demand per route (dial host, dial port, TLS server
// name, ALPN offer); none exist before the first request.
struct http_client_connection_limits final {
    // Physical connections of every route and protocol, including dialing and
    // idle ones (like Go's MaxConnsPerHost). At the limit, the least recently
    // used idle connection is closed for a new dial; without one, requests wait
    // within acquire_timeout_. nullopt leaves the count unbounded.
    std::optional<std::size_t> max_open_;
    // Idle connections retained across all routes; beyond these, the least
    // recently used idle connection of that kind closes at once.
    std::size_t max_idle_http1_{64};
    // HTTP/2 and HTTP/3 connections without streams.
    std::size_t max_idle_multiplexed_{64};
};

// Per-origin configuration. Standalone owners and application workers use the
// same isolated transport, TLS, cookie and connection limits for each alias.
struct http_client_config final {
    borrowed_text alias_{"default"};
    borrowed_text base_url_{};
    // 仅控制物理拨号；逻辑 origin、默认 SNI 和证书验证名来自 base_url_，
    // 请求可经 http_client_request_options 逐请求覆盖。
    std::optional<borrowed_text> dial_host_;
    std::optional<port_number> dial_port_;
    // 接收已经解析的数值地址；拒绝的候选绝不交给 connect。配置可复制并由
    // 多个 worker 共享，故共享同一目标。
    std::shared_ptr<const http_client_address_validator> address_validator_;
    // Each period looks up again only the DNS dial targets that still have a
    // live connection (the alias's and request dial_host_ overrides), within
    // http_client_worker_config::max_concurrent_dns_refreshes_, and retires
    // connections to addresses an answer no longer contains.
    std::optional<timeout_duration> dns_refresh_interval_;
    bool decode_content_{true};
    http_client_connection_limits connection_limits_{};
    std::size_t max_concurrent_http2_streams_per_connection_{100};
    std::size_t max_buffered_requests_{1024};
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
    timeout_duration connect_timeout_{5000};
    // Bound of each transport write. An ordinary HTTP/1 request head and
    // immediate content share one write; streamed upload chunks remain separate.
    std::optional<timeout_duration> write_timeout_{30000};
    // Inactivity bound of a response read, restarted by every read that
    // receives bytes: the response head, body, or an HTTP/2/3 stream's DATA.
    // Until the final response head, sending request content restarts it too:
    // an origin may read the complete request before it answers. A response
    // read that waits for this client's consumer to drain buffered body
    // (HTTP/2/3: to return flow-control credit) is not inactive. Accepted
    // tunnels use http_client_tunnel_config::idle_timeout_ instead. Expiry
    // fails the response with timeout within request_timeout_.
    std::optional<timeout_duration> read_timeout_{30000};
    // Total deadline of one exchange, from admission through connect,
    // request transfer, response head and the complete body, including the
    // time the body producer waits for a slow consumer (read(), pipe_to()) to
    // drain max_response_bytes_ of buffered body. Expiry fails the response
    // with timeout and discards its connection. An operation's own timeout
    // replaces it; nullopt leaves the exchange unbounded.
    // Accepted WebSockets have no implicit total lifetime limit; their active
    // operations retain read/write, heartbeat, close and explicit operation bounds.
    std::optional<timeout_duration> request_timeout_{30000};
    // Bound of the wait for connection admission, within request_timeout_:
    // a max_open_ slot, a connection of the request's route still negotiating
    // its protocol, a draining HTTP/2 session retiring before its reconnect, and
    // HTTP/2 stream capacity below the peer's SETTINGS_MAX_CONCURRENT_STREAMS.
    // Applies even when request_timeout_ is nullopt.
    std::optional<timeout_duration> acquire_timeout_{5000};
    // A pooled HTTP/1 connection between exchanges, or an HTTP/2 connection
    // without streams, closes once idle this long. Independently, an idle
    // HTTP/1 connection closes as soon as its peer ends it or sends bytes.
    std::optional<timeout_duration> idle_timeout_{90000};
    // HTTP/2 connection liveness: after this long without any received frame
    // the client sends a PING, and fails the connection with timeout if
    // another interval passes without input; nullopt sends no PING.
    std::optional<timeout_duration> http2_ping_interval_{};
    std::size_t max_response_bytes_{default_max_buffered_body_bytes};
    // Default versions of the alias's requests.
    http_client_versions versions_{};
    http_client_advertisement_config advertisements_{};
    http_client_push_config push_{};
    http_client_http3_config http3_{};
    // Used for https; client_tls_mode::disabled is rejected (select http instead).
    client_tls_config tls_{};
    client_tcp_config tcp_{};
    http_client_received_cookie_policy received_cookies_{http_client_received_cookie_policy::ignore};
    // Sent as the User-Agent of a request or handshake that carries no
    // User-Agent field; a caller field replaces it and an empty value sends
    // none. Must be a valid header field value.
    borrowed_text user_agent_{"Ruvia"};
    // Client-stored like retained cookies: never sent with TRACE (RFC 9110
    // §9.3.8) or with a tunnel to another authority (RFC 6265 §5.4). A later
    // entry replaces an earlier one of the same name; max_cookies_ and
    // max_cookie_bytes_ bound the result. Explicit caller Cookie fields are
    // combined independently of the stored jar.
    std::vector<http_header_view> cookies_;
};

// Protocol and connection-pool failures of the HTTP client. An operation's
// timeout or cancellation is not among them: the client reports it as
// std::errc::timed_out / std::errc::operation_canceled (generic_category),
// exactly like server operations, so one std::system_error handler covers both.
enum class http_client_errc : std::uint8_t {
    not_configured = 1,
    invalid_request,
    resolve_failed,
    connect_failed,
    tls_failed,
    protocol_unavailable,
    io_error,
    protocol_error,
    response_too_large,
    queue_full,
    closing,
    result_budget_exceeded,
    handshake_rejected,
    message_too_large,
};

[[nodiscard]] const std::error_category& http_client_category() noexcept;

[[nodiscard]] inline std::error_code make_error_code(http_client_errc value) noexcept {
    return {static_cast<int>(value), http_client_category()};
}

// code() is an http_client_errc, or std::errc::timed_out /
// std::errc::operation_canceled for an interrupted operation.
class http_client_error final : public std::system_error {
public:
    http_client_error(std::error_code code, std::string_view message,
        std::optional<http_status_code> status = std::nullopt,
        const http_client_connection_info* connection = nullptr, bool request_written = true,
        std::size_t redirect_hops = 0)
        : std::system_error(code, std::string(message)),
          status_(status),
          redirect_hops_(redirect_hops),
          request_written_(request_written) {
        capture_connection(connection);
    }
    http_client_error(const http_client_error& other)
        : std::system_error(other),
          status_(other.status_),
          redirect_hops_(other.redirect_hops_),
          request_written_(other.request_written_) {
        capture_connection(other.connection_info());
    }
    http_client_error(http_client_error&&) noexcept = default;
    http_client_error& operator=(const http_client_error& other) {
        std::system_error::operator=(other);
        status_ = other.status_;
        redirect_hops_ = other.redirect_hops_;
        request_written_ = other.request_written_;
        capture_connection(other.connection_info());
        return *this;
    }
    http_client_error& operator=(http_client_error&&) = default;

    [[nodiscard]] std::optional<http_status_code> status() const noexcept {
        return status_;
    }
    /// 已建立连接的失败保留快照；拨号尚未建立连接时为空。
    [[nodiscard]] const http_client_connection_info* connection_info() const& noexcept RUVIA_LIFETIMEBOUND {
        return connection_ ? &*connection_ : nullptr;
    }
    const http_client_connection_info* connection_info() const&& = delete;
    /// false 保证本请求尚未向 transport 提交任何字节（排队、拨号、TLS 阶段）。
    /// true 表示可能已有字节到达对端，超时与取消同样保留该保守状态。
    /// HTTP/2 REFUSED_STREAM / GOAWAY 与 HTTP/3 request_unprocessed 只保证
    /// 请求未被处理，不抹去已有写出进度；协议层的安全重试独立遵循此声明。
    [[nodiscard]] bool request_written() const noexcept {
        return request_written_;
    }
    [[nodiscard]] std::size_t redirect_hops() const noexcept {
        return redirect_hops_;
    }
    [[nodiscard]] bool followed_redirect() const noexcept {
        return redirect_hops_ != 0;
    }

private:
    void capture_connection(const http_client_connection_info* connection) {
        if (connection == nullptr) {
            connection_.reset();
            return;
        }
        if (!connection_) {
            connection_.emplace();
        }
        // 错误可比 response 存活更久，文本独立拥有。
        *connection_ = *connection;
    }
    std::optional<http_status_code> status_;
    std::optional<http_client_connection_info> connection_;
    std::size_t redirect_hops_{0};
    bool request_written_;
};

struct http_client_stats final {
    // Requests waiting for a connection: a max_open_ slot, a connection of
    // their route still negotiating, HTTP/2/3 stream capacity, or an HTTP/1
    // connection's serial exchange. Bounded by max_buffered_requests_.
    std::size_t buffered_requests_{0};
    // Requests a connection carries.
    std::size_t in_flight_requests_{0};
    std::size_t completed_requests_{0};
    std::size_t failed_requests_{0};
    // Plaintext application octets completed at the transport's HTTP/tunnel
    // boundary, including a completed prefix reported with an I/O error. Includes
    // HTTP framing and tunnel payload; excludes TLS handshake/record overhead and
    // content-decoding expansion.
    std::size_t plaintext_bytes_sent_{0};
    std::size_t plaintext_bytes_received_{0};
    // Idle HTTP/1 connections retired because transport bytes or a buffered TLS
    // record arrived while they idled, observed at once or before the next
    // request. A TLS record can carry close_notify, post-handshake control data
    // or an unsolicited response.
    std::size_t idle_connections_with_pending_data_{0};
    // Connection owners of every route and protocol: dialing, in use, idle, or
    // closed while a request still unwinds; reclaimed once nothing borrows them.
    std::size_t open_connections_{0};
    // HTTP/1 connections between exchanges and HTTP/2/3 ones without streams.
    std::size_t idle_connections_{0};
    // Background DNS refresh lookups started: one per refresh period for each
    // DNS dial target that still has a live connection.
    std::size_t dns_refreshes_{0};
    std::size_t dropped_advertisements_{0};
    std::size_t received_pushes_{0};
    std::size_t rejected_pushes_{0};
};

}  // namespace ruvia

template <>
struct std::is_error_code_enum<ruvia::http_client_errc> : std::true_type {};

namespace ruvia {

class event_loop;
namespace detail {
class tcp_client_state;
}

struct tcp_client_config final {
    // An unbracketed IP literal or ASCII DNS name. DNS requires a blocking pool
    // on the bound event loop. Configuration is owned before construction returns.
    std::string host_;
    port_number port_{};
    client_tls_config tls_{};
    client_tcp_config tcp_{};
    // RFC 7301 opaque protocol names, in preference order: 1..255 bytes each,
    // at most 65535 encoded bytes in total. Non-empty offers require TLS.
    std::vector<std::string> alpn_;
    std::optional<timeout_duration> connect_timeout_{std::chrono::seconds(30)};
    std::optional<timeout_duration> read_timeout_{};
    std::optional<timeout_duration> write_timeout_{};
};

// Address-stable owner of one outbound byte stream, with the shared DNS,
// Happy Eyeballs and SAN-authenticating TLS/mTLS transport.
// Operations run on worker(); construction, close and destruction may run on
// another thread. Connect once; one read and one write may run concurrently,
// but overlapping operations in the same direction throw std::logic_error.
// Timeouts begin when awaited; per-operation options may tighten the configured
// timeout. Cancellation or I/O failure closes both directions, never permitting
// reuse of a partially transmitted TLS record. I/O failures throw std::system_error
// (timed_out / operation_canceled for deadlines / cancellation).
// Cold tasks retain their input and owner; shutdown and loop stop retire cold
// read/write operations and join running ones before retiring the transport.
class tcp_client final {
public:
    tcp_client(event_loop loop, const tcp_client_config& config);
    ~tcp_client();
    tcp_client(const tcp_client&) = delete;
    tcp_client& operator=(const tcp_client&) = delete;
    tcp_client(tcp_client&&) = delete;
    tcp_client& operator=(tcp_client&&) = delete;

    [[nodiscard]] task<void> connect(operation_options options = {}) &;
    task<void> connect(operation_options = {}) && = delete;
    // Owner loop, after connect(). Empty for TCP or if the peer skipped ALPN.
    // The view borrows this connection until close, shutdown or I/O failure.
    [[nodiscard]] std::string_view alpn() const& RUVIA_LIFETIMEBOUND;
    std::string_view alpn() const&& = delete;
    // At most 4 KiB. nullopt is EOF (TCP FIN, TLS close_notify or truncation).
    // Borrowed bytes remain valid until the next read starts or shutdown joins;
    // retain this owner while using the view. EOF does not end the write lane.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options options = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    // Owns the bytes before returning the cold task; completes after all bytes
    // are written. Failure may have sent a prefix and always retires the stream.
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes, operation_options options = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::string_view bytes, operation_options options = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    // Exact std::string rvalues transfer storage directly without copying.
    template <typename string_type>
        requires std::same_as<string_type, std::string>
    [[nodiscard]] task<void> write(string_type&& bytes, operation_options options = {}) & {
        return write_owned(std::move(bytes), std::move(options));
    }
    template <typename string_type>
        requires std::same_as<string_type, std::string>
    task<void> write(string_type&&, operation_options = {}) && = delete;

    // Immediate whole-connection cancellation, not a send half-close. TLS uses
    // the shared transport's bounded best-effort close_notify before closing.
    void close() noexcept;
    // Closes immediately and waits for operation and socket retirement. This
    // is not a TLS graceful shutdown or a send half-close. Run on worker().
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    [[nodiscard]] task<void> write_owned(std::string bytes, operation_options options);
    std::shared_ptr<detail::tcp_client_state> state_;
};

}  // namespace ruvia
