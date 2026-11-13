#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_content_semantics.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/port_number.h"
#include "ruvia/web/client_tcp_config.h"
#include "ruvia/web/tls_config.h"
namespace ruvia {

enum class http_client_protocol : std::uint8_t {
    negotiate,
    http1_only,
    http2_only,
    http3_only,
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

using http_client_informational_callback = move_only_function<task<void>(http_status_code, std::span<const http_header>, stop_token)>;

struct http_client_request_options final {
    // nullopt 继承 origin 默认；false 为保留编码字段的原样流。
    std::optional<bool> decode_content_;
    http_request_field_source field_source_{http_request_field_source::generated};
    // 独立物理连接，既不借业务连接，也不归还业务复用池。
    bool fresh_connection_{false};
    http_client_phase_timeouts timeouts_;
    std::shared_ptr<http_client_informational_callback> informational_;
    // 请求级路由覆盖；未设字段继承 alias，scheme 始终来自 alias 的 base_url_。
    // authority_：逻辑 Host / :authority（host[:port]，无 userinfo）。不同于
    // alias authority 时，Cookie jar 与 Alt-Svc 不参与，跨 origin 重定向丢弃全部覆盖。
    std::optional<std::pmr::string> authority_;
    // 物理拨号目标；域名只在新建连接时解析，并经 address_validator_ 校验。
    std::optional<std::pmr::string> dial_host_;
    std::optional<port_number> dial_port_;
    // TLS SNI 与证书校验名，默认取有效 authority 的 host（IP 字面量不发 SNI，
    // 按 IP SAN 校验）。连接按 (拨号 host, 拨号端口, server name, ALPN) 复用，
    // Host / :authority 不入键：HTTP/2/3 连接上发送其他 authority 时，调用方
    // 负责该证书覆盖它（RFC 9113 §9.1.1、RFC 9114 §3.3）。
    std::optional<std::pmr::string> server_name_;
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
    explicit http_client_connection_info(std::pmr::memory_resource* resource = process_resource())
        : peer_ip_(pmr_resource_or_default(resource)) {}
    http_client_connection_info(const http_client_connection_info& other)
        : http_client_connection_info(other.peer_ip_.get_allocator().resource()) {
        *this = other;
    }
    http_client_connection_info(http_client_connection_info&&) noexcept = default;
    http_client_connection_info& operator=(const http_client_connection_info&) = default;
    http_client_connection_info& operator=(http_client_connection_info&&) = default;

    // 物理连接身份，不是槽位。重新拨号产生新 id，旧 id 不能关闭新连接。
    std::uint64_t id_{};
    std::pmr::string peer_ip_{process_resource()};
    std::uint16_t peer_port_{};
    bool reused_{false};
    std::chrono::steady_clock::duration connect_duration_{};
    std::chrono::steady_clock::duration tls_duration_{};
};

struct http_client_endpoint_address final {
    explicit http_client_endpoint_address(std::pmr::memory_resource* resource = process_resource())
        : ip_(pmr_resource_or_default(resource)) {}
    http_client_endpoint_address(std::string_view ip, port_number port,
        std::pmr::memory_resource* resource = process_resource())
        : ip_(ip, pmr_resource_or_default(resource)),
          port_(port) {}
    http_client_endpoint_address(const http_client_endpoint_address& other)
        : http_client_endpoint_address(other.ip_, other.port_, other.ip_.get_allocator().resource()) {}
    http_client_endpoint_address(http_client_endpoint_address&&) noexcept = default;
    http_client_endpoint_address& operator=(const http_client_endpoint_address&) = default;
    http_client_endpoint_address& operator=(http_client_endpoint_address&&) = default;

    std::pmr::string ip_{process_resource()};
    port_number port_{};
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
    // 接收已经解析的数值地址；拒绝的候选绝不交给 connect。
    std::function<bool(std::string_view, std::uint16_t)> address_validator_;
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
    // receives bytes: the HTTP/1 response head, body or tunnel bytes, or an
    // HTTP/2 stream's head or DATA. A response read that waits for this
    // client's consumer to drain buffered body (HTTP/2: to return flow-control
    // credit) is not inactive. Reads during a streaming upload are bounded too.
    // Expiry fails the response with timeout within request_timeout_.
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
    http_client_protocol protocol_{http_client_protocol::negotiate};
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
    std::pmr::vector<http_header_view> cookies_{process_resource()};
};

class http_client_error final : public std::runtime_error {
public:
    enum class code_type : std::uint8_t {
        not_configured,
        invalid_request,
        timeout,
        cancelled,
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

    http_client_error(code_type code, std::string_view message, std::optional<http_status_code> status = std::nullopt,
        const http_client_connection_info* connection = nullptr)
        : std::runtime_error(std::string(message)),
          code_(code),
          status_(status) {
        capture_connection(connection);
    }
    http_client_error(const http_client_error& other)
        : std::runtime_error(other),
          code_(other.code_),
          status_(other.status_) {
        capture_connection(other.connection_info());
    }
    http_client_error(http_client_error&&) noexcept = default;
    http_client_error& operator=(const http_client_error& other) {
        std::runtime_error::operator=(other);
        code_ = other.code_;
        status_ = other.status_;
        capture_connection(other.connection_info());
        return *this;
    }
    http_client_error& operator=(http_client_error&&) = default;

    [[nodiscard]] code_type code() const noexcept {
        return code_;
    }
    [[nodiscard]] std::optional<http_status_code> status() const noexcept {
        return status_;
    }
    /// 已建立连接的失败保留快照；拨号尚未建立连接时为空。
    [[nodiscard]] const http_client_connection_info* connection_info() const& noexcept RUVIA_LIFETIMEBOUND {
        return connection_ ? &*connection_ : nullptr;
    }
    const http_client_connection_info* connection_info() const&& = delete;

private:
    void capture_connection(const http_client_connection_info* connection) {
        if (connection == nullptr) {
            connection_.reset();
            return;
        }
        if (!connection_) {
            connection_.emplace();
        }
        // 错误可比 response 存活更久，文本提升到 process_resource。
        *connection_ = *connection;
    }
    code_type code_;
    std::optional<http_status_code> status_;
    std::optional<http_client_connection_info> connection_;
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
    std::size_t dropped_advertisements_{0};
    std::size_t received_pushes_{0};
    std::size_t rejected_pushes_{0};
};

}  // namespace ruvia
