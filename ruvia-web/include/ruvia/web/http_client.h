#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/web/model.h"
#include "ruvia/web/streaming.h"

namespace ruvia {

// One certificate chain and its private key, from exactly one source: two PEM
// files, or the same PEM text in memory (the leaf certificate followed by its
// chain, and the key). Mixing the sources, or naming only half of one, throws
// std::invalid_argument. Paths must not be empty or contain NUL; they reach
// OpenSSL and the database drivers as UTF-8, so a native Windows name never
// narrows through the ANSI code page. In-memory PEM serves HTTPS listeners
// only: database, Redis and HTTP clients read identity files and reject it.
// A non-empty password decrypts the key; its binary bytes, including
// embedded NUL, are preserved. Listener configuration stages these bytes;
// starting/adding a listener, replacing identities and certificate-store
// updates parse once off-worker and cleanse their owned password/PEM copies.
struct tls_identity_config final {
    std::filesystem::path certificate_chain_file_{};
    std::filesystem::path private_key_file_{};
    std::string certificate_chain_pem_{};
    std::string private_key_pem_{};
    std::string private_key_password_{};
};

using tls_certificate_id = std::uint64_t;

// Immutable ClientHello views, valid only during the selection callback.
// Wire lists are decoded into host-order uint16 protocol identifiers and ALPN
// protocol name views. Absent extensions have empty views. No OpenSSL object,
// file access or borrowed application identity is needed.
struct tls_certificate_selection final {
    std::uint64_t connection_id_{};
    ip_address remote_address_;
    std::uint16_t remote_port_{};
    std::string_view server_name_;
    std::optional<ip_address> local_address_;
    std::uint16_t local_port_{};
    bool quic_{};
    std::uint16_t legacy_version_{};
    std::span<const std::uint16_t> cipher_suites_;
    std::span<const std::uint16_t> signature_algorithms_;
    std::span<const std::uint16_t> supported_versions_;
    std::span<const std::uint16_t> supported_groups_;
    std::span<const std::string_view> alpn_;

    [[nodiscard]] bool offers_signature_algorithm(std::uint16_t algorithm) const noexcept;
};

// nullopt rejects the handshake. Exceptions and IDs absent from the snapshot
// also fail closed, including on resumed handshakes. Calls may run concurrently
// on different workers; the callable must synchronize its own mutable state.
using tls_certificate_selector = move_only_function<std::optional<tls_certificate_id>(const tls_certificate_selection&)>;

namespace detail {
struct tls_certificate_store_access;
}

// Thread-safe, independently usable certificate library. Writers serialize
// without blocking workers, prepare immutable snapshots on caller's blocking
// pool, then atomically publish on caller. Failed/rejected updates leave the
// library unchanged. Handshakes capture a snapshot before calling the selector:
// replacing/erasing its selected ID cannot invalidate that handshake.
// Established connections retain their OpenSSL references. Cold tasks retain
// the library state and own their inputs even after this object is destroyed.
// Copies are deliberately disabled; tls_config retains shared ownership.
class tls_certificate_store final {
public:
    tls_certificate_store();
    ~tls_certificate_store();
    tls_certificate_store(const tls_certificate_store&) = delete;
    tls_certificate_store& operator=(const tls_certificate_store&) = delete;

    // Await on caller; awaiting elsewhere throws std::logic_error. The caller
    // must have an attached blocking_pool for parsing or snapshot preparation.
    // Rejections throw blocking_operation_rejected; loader exceptions propagate
    // unchanged. A stopping caller cannot publish: std::runtime_error before
    // submission/after preparation, or blocking_operation_rejected while
    // waiting for the pool. Started tasks must be awaited/joined.
    // ID zero throws std::invalid_argument. Before returning a cold task, the
    // library copies secrets into heap storage and cleanses its
    // incoming copy, including on failure. Discarding a cold task cleanses its
    // retained copy; any detached preparation owns and cleanses its own input
    // and never publishes. These operations have no per-call timeout/token.
    [[nodiscard]] task<void> insert_or_assign(
        worker_ref caller, tls_certificate_id id, tls_identity_config identity);
    // Returns false for an absent ID without submitting blocking work.
    [[nodiscard]] task<bool> erase(worker_ref caller, tls_certificate_id id);
    [[nodiscard]] bool contains(tls_certificate_id id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    friend struct detail::tls_certificate_store_access;
    struct state;
    std::shared_ptr<state> state_;
};

enum class client_tls_mode : std::uint8_t {
    verify_identity,
    // Plaintext.
    disabled,
};

// TLS wire ProtocolVersion values; older versions are deliberately unsupported.
enum class tls_version : std::uint16_t {
    tls_1_1 = 0x0302,
    tls_1_2 = 0x0303,
    tls_1_3 = 0x0304,
};

// The one client TLS shape shared by database and Redis clients. TLS always
// authenticates the peer. Redis clients read the CA bundle and identity once,
// when the registration is created, and share them across workers; database
// drivers read the files when connecting and do not accept an encrypted
// private key. Clients default to TLS 1.1 or newer for legacy compatibility;
// minimum_version_ can require TLS 1.2 or 1.3. TLS 1.2 offers only ECDHE
// with AEAD suites, and every version refuses renegotiation. Every client verifies
// identities by subjectAltName type (RFC 9525): DNS names match DNS-ID entries,
// IP literals match IP entries, the subject Common Name is never used, and DNS
// wildcards cover only a complete left-most label.
struct client_tls_config final {
    client_tls_mode mode_{client_tls_mode::verify_identity};
    // Shared by HTTP, raw TCP, Redis and database clients. HTTP/2 and QUIC
    // additionally enforce their own protocol minimum, never lowering this one.
    tls_version minimum_version_{tls_version::tls_1_1};
    // The CA bundle that authenticates the server; nullopt selects the system
    // or driver trust store.
    std::optional<std::filesystem::path> ca_file_{};
    // The certificate this client presents for mutual TLS.
    std::optional<tls_identity_config> identity_{};
    // SNI and verified peer name, an IP literal or ASCII DNS name; nullopt
    // uses the configured host. An IP literal matches IP subjectAltNames.
    std::optional<std::string> server_name_{};
    // 每条 TLS 连接发出的记录总上限（含握手和 KeyUpdate），范围 1..2^23。
    // 达到上限后终止连接；更新密钥不重置计数。保守的连接级上限保证不会达到
    // RFC 9846 §5.5 的单密钥 AES-GCM 用量上限，也适用于数据库连接。
    std::uint64_t max_outgoing_records_{std::uint64_t{1} << 23};
};

enum class tls_client_certificate_requirement : std::uint8_t {
    optional,
    required,
};

struct tls_client_certificate_config final {
    // The CA bundle that verifies presented client certificates. Optional mode
    // admits a client without a certificate; required mode rejects it.
    std::optional<std::filesystem::path> ca_file_{};
    tls_client_certificate_requirement requirement_{tls_client_certificate_requirement::optional};
};

// An additional identity served when the client's server_name (RFC 6066 SNI)
// equals host_, compared ASCII case-insensitively. A host_ "*.example.com" is
// a wildcard covering exactly one whole left-most label (RFC 6125 §6.4.3):
// "a.example.com", but neither "example.com" nor "a.b.example.com". An exact
// host_ wins over a wildcard covering the same name.
struct tls_sni_config final {
    std::string host_{};
    tls_identity_config identity_{};
    // This identity's stapled OCSP response; see tls_config::ocsp_response_.
    std::vector<std::byte> ocsp_response_{};
};

// An additional identity served on connections that arrived at address_, a
// numeric IP address that is not unspecified (an IPv4-mapped IPv6 address
// equals its IPv4 form), when the client sent no server_name or one matching
// no sni_ entry -- typically a listener bound to a wildcard address serving
// clients that connect by IP literal, which RFC 6066 forbids naming in SNI.
// Both TLS over TCP and HTTP/3 use the actual destination address, including
// wildcard listeners; QUIC retains packet destination/interface information
// when handing datagrams between workers and selecting the reply source.
struct tls_local_address_config final {
    std::string address_{};
    tls_identity_config identity_{};
    // This identity's stapled OCSP response; see tls_config::ocsp_response_.
    std::vector<std::byte> ocsp_response_{};
};

// One HTTPS listener's identities. The listener negotiates TLS 1.1, 1.2 or 1.3;
// its TLS 1.2 suites are ECDHE with AEAD only, chosen by server preference;
// renegotiation is refused; ALPN selects h2, http/1.1 or http/1.0, with HTTP/2
// requiring TLS 1.2 or newer. The paired HTTP/3 service (listen_config::http3_)
// uses the same identities over QUIC with TLS 1.3 only and ALPN h3 only.
// Without certificate_store_, each handshake selects once, when its
// ClientHello is processed: an exact sni_ host, else a
// wildcard sni_ host, else a local_addresses_ entry, else the primary
// identity_. A client offering no server_name, or one matching nothing, is
// thus still served: a product choice for single-certificate and wildcard
// deployments, deliberately not RFC 9325 §3.7's unrecognized_name alert.
// Sessions resume across workers of the listener, in every supported TLS
// version only under the server_name (compared ASCII case-insensitively) they
// were established with, and across application::replace_tls_identities(). Ticket
// keys rotate every two hours (the session lifetime) and are destroyed one
// interval after their last use for new tickets. RFC 9110 §7.4: a request
// whose target is an "https" resource is answered 421 (Misdirected Request)
// unless the certificate this connection presented names its host in a
// subjectAltName DNS or IP entry (whole-label wildcards only; the subject CN
// is ignored); on HTTP/2 only that stream is refused. Cleartext listeners
// answer 421 to "https" targets. Peers in trusted_proxy_config are exempt
// from both checks.
struct tls_config final {
    tls_identity_config identity_{};
    // The DER OCSPResponse (RFC 6960 §4.2.1) stapled for identity_ (RFC 6066
    // §8 status_request; in TLS 1.3 the leaf's CertificateEntry extension,
    // RFC 8446 §4.4.2.1), sent only to a client that requests OCSP status and
    // only with the identity it belongs to; each sni_ and local_addresses_
    // entry carries its own. Empty staples nothing. Invalid DER (including
    // trailing bytes or BER) throws std::invalid_argument when identities
    // load. The bytes are served as given; the caller owns certificate
    // association, signature and freshness checks. No fetching or refresh:
    // replace_tls_identities() installs a renewed response with its identity.
    // Not available with certificate_store_.
    std::vector<std::byte> ocsp_response_{};
    tls_client_certificate_config client_certificates_{};
    std::vector<tls_sni_config> sni_{};
    std::vector<tls_local_address_config> local_addresses_{};
    // 发出的 TLS 记录总上限，范围 1..2^23；含握手及控制记录，KeyUpdate 不
    // 重置计数。达到上限后终止连接，以满足 RFC 9846 §5.5 的密钥用量约束。
    std::uint64_t max_outgoing_records_{std::uint64_t{1} << 23};
    // The ALPN protocols (RFC 7301) a listener_dispatch::connection listener
    // offers, in server preference order; empty offers h2, then http/1.1.
    // Each name is 1..255 bytes. A client offering ALPN with none of them
    // fails the handshake with no_application_protocol (RFC 7301 §3.2); one
    // offering no ALPN completes it without a protocol. A routes listener
    // negotiates its own HTTP versions and rejects a non-empty list.
    std::vector<std::string> alpn_{};
    // Optional live library, shared by every worker and both TLS transports.
    // With a library, fixed identity_/sni_/local_addresses_ must be empty.
    // Without a selector, default_certificate_ selects an ID. A selector must
    // be paired with a library and completely replaces fixed selection.
    // Updating the library never rebuilds contexts, ticket keys or caches.
    std::shared_ptr<tls_certificate_store> certificate_store_;
    std::shared_ptr<const tls_certificate_selector> select_certificate_;
    tls_certificate_id default_certificate_{};
};

}  // namespace ruvia

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

namespace ruvia {

class context;
class context_response_editor;
class request_stream;
class response_stream_writer;
class http_client_request;
class http_client_connect_request;
class http_client_websocket_request;
class operation_deadline;

namespace detail {
class http_client_pool;
class http_client_registry;
class http_client_response_handoff;
class http_client_response_state;
class http_client_advertisement_queue;
struct http_client_advertisement_state;
class http_capsule_stream_state;
class http_client_result_budget_domain;
class http_client_result_budget_lease;
class http_client_state;
class http_client_websocket_transport;
class worker_client_registry_view;
}  // namespace detail

// Immutable metadata retained by its http_client_response. Field views borrow
// that response and remain valid through subsequent body reads.
// HTTP/1 transfers its already-owned parsed fields without another field copy.
// Other protocol resources are copied into response-owned storage.
class http_client_informational_response final {
public:
    http_client_informational_response(const http_client_informational_response&) = delete;
    http_client_informational_response& operator=(const http_client_informational_response&) = delete;
    http_client_informational_response(http_client_informational_response&&) noexcept = default;
    http_client_informational_response& operator=(http_client_informational_response&&) = delete;
    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_;
    }
    std::span<const http_header> headers() const&& = delete;

private:
    friend class detail::http_client_response_state;
    explicit http_client_informational_response(http_status_code status) noexcept
        : status_(status) {}
    http_status_code status_;
    std::vector<http_header> headers_;
};

// Independently owned bytes collected from an outbound HTTP response body.
// The address-stable backing storage is detached from the client and worker
// lifecycle, and its read-only view remains valid across moves until destruction.
// Empty results retain neither backing storage nor a retained-byte reservation.
class http_client_response_bytes final {
public:
    http_client_response_bytes(const http_client_response_bytes&) = delete;
    http_client_response_bytes& operator=(const http_client_response_bytes&) = delete;
    http_client_response_bytes(http_client_response_bytes&&) noexcept;
    http_client_response_bytes& operator=(http_client_response_bytes&&) noexcept;
    ~http_client_response_bytes();

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return {data_.get(), size_};
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        return {reinterpret_cast<const char*>(data_.get()), size_};
    }
    std::string_view text() const&& = delete;

private:
    friend class detail::http_client_response_state;

    http_client_response_bytes(
        std::size_t reserve_bytes, detail::http_client_result_budget_lease&& lease_value);
    http_client_response_bytes() noexcept = default;
    void append(std::span<const std::byte> bytes) noexcept;
    void release() noexcept;

    std::shared_ptr<detail::http_client_result_budget_domain> budget_domain_{};
    std::unique_ptr<std::byte[]> data_{};
    std::size_t size_{0};
    std::size_t reserved_{0};
};

// A borrow-only facade embedded in http_client_response. It cannot be detached,
// independently destroyed, or moved away from the response. Body operations
// bind directly to the response's address-stable transport state, so moving
// their public response owner does not invalidate a cold or running operation.
// HTTP/1 body-suppressed and known-zero-length responses do not allocate a
// framing-decoder scratch block; other response storage and budgets still apply.
// A body whose Content-Encoding stack is fully supported (gzip, deflate, br,
// zstd) is decoded before the first byte is exposed, and its Content-Encoding
// and encoded Content-Length fields are removed (RFC 9110 §8.4). A 206 body, or
// a stack containing an unknown coding, is exposed as received with its
// Content-Encoding field kept; a malformed or over-deep Content-Encoding field
// fails the response with protocol_error.
// decode_content(false) 保留编码字节与原 Content-Encoding/Content-Length，
// 不等正文结束即交付第一块；framing()/content_length() 始终表示线面计划。
// For HTTP/1, bytes received beyond a complete framed response retire the
// connection and are discarded; they do not turn that response into a failure.
//
// Every body operation takes its own operation_options, timed from when it
// starts; a non-positive timeout throws std::invalid_argument at the call. Its
// expiry or stop request ends only that wait with http_client_error
// (std::errc::timed_out / operation_canceled): buffered bytes and the read lane
// remain, so reading may resume.
// The request's own deadlines and host cancellation keep bounding the exchange.
class http_client_response_body final {
public:
    http_client_response_body(const http_client_response_body&) = delete;
    http_client_response_body& operator=(const http_client_response_body&) = delete;
    http_client_response_body(http_client_response_body&&) = delete;
    http_client_response_body& operator=(http_client_response_body&&) = delete;

    // The returned view remains valid until the next body operation starts. A null
    // optional is the only end-of-body signal; an empty data chunk is never
    // returned. Reads are linear and concurrent operations are rejected.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;

    // Reads one chunk as characters, without decoding or charset validation.
    // Encoded characters may straddle chunks. Uses the same linear read lane.
    [[nodiscard]] task<std::optional<std::string_view>> text(operation_options operation = {}) &;
    task<std::optional<std::string_view>> text(operation_options = {}) && = delete;

    // Collects the unread remainder of this same stream. max_bytes is a caller
    // bound in addition to the origin's transport bound. An interrupted
    // collection keeps the bytes received so far for the next body operation.
    [[nodiscard]] task<http_client_response_bytes> read_all(
        std::size_t max_bytes = default_max_buffered_body_bytes, operation_options operation = {}) &;
    task<http_client_response_bytes> read_all(
        std::size_t = default_max_buffered_body_bytes, operation_options = {}) && = delete;

    // 以自然背压转发当前上游正文；调用者必须先对 output 执行 commit()，
    // 只有 body 结果才启动转发，completed 时不得消费/生产转发正文。
    // operation 约束整个转发；中断抛出 errc::timed_out / operation_canceled，已提交的下游
    // 输出按流式提交契约随该异常中止，上游正文游标停在最后一块成功写出处。
    [[nodiscard]] task<void> pipe_to(response_stream_writer& output, operation_options operation = {}) &;
    task<void> pipe_to(response_stream_writer&, operation_options = {}) && = delete;
    // 尚有未读正文或接收尚未终止时返回 false；耗尽后仅正常完成返回 true。
    // 失败时与 read() 一样抛出，包括客户端提前关闭；独立上传失败不影响
    // 已完整接收的响应。移动后不再持有状态的响应仍返回 true。
    [[nodiscard]] bool complete() const;

private:
    friend class http_client_response;

    ~http_client_response_body() = default;

    explicit http_client_response_body(detail::http_client_response_state* state_value) noexcept
        : state_(state_value) {}

    detail::http_client_response_state* state_{nullptr};
};

// Owns worker-affine response storage independently of its client transport.
// May survive client shutdown/destruction on that worker, but must be destroyed
// before the bound event_loop retires. Metadata views borrow this response;
// body chunks remain valid until the next body operation starts.
class http_client_response final {
public:
    http_client_response(const http_client_response&) = delete;
    http_client_response& operator=(const http_client_response&) = delete;
    // Body operations bind to the address-stable response state, so the public
    // owner remains movable while an operation is cold or running.
    http_client_response(http_client_response&& other) noexcept;
    http_client_response& operator=(http_client_response&& other) noexcept;
    ~http_client_response();

    // Informational heads are retained in wire order without a count cap.
    // Aggregate field-byte and in-flight allocation budgets still apply.
    [[nodiscard]] std::span<const http_client_informational_response> informational_responses() const& RUVIA_LIFETIMEBOUND;
    std::span<const http_client_informational_response> informational_responses() const&& = delete;
    // The status the response is processed as; a received 600..999 is
    // processed as 500 (RFC 9110 section 15).
    [[nodiscard]] http_status_code status() const;
    [[nodiscard]] bool ok() const {
        return status().value() >= 200 && status().value() < 300;
    }
    // The status code as received, including an invalid 600..999.
    [[nodiscard]] http_received_status received_status() const;
    [[nodiscard]] http_protocol_version protocol_version() const;
    [[nodiscard]] http_client_response_framing framing() const;
    [[nodiscard]] std::optional<std::uint64_t> content_length() const;
    [[nodiscard]] const http_client_connection_info& connection_info() const& RUVIA_LIFETIMEBOUND;
    const http_client_connection_info& connection_info() const&& = delete;
    // Queues RFC 9218 PRIORITY_UPDATE on a live HTTP/2 response; a
    // response the peer has already completed is left unchanged. Throws
    // http_client_error(closing) once the response transport is retired.
    // Worker-affine; the connection driver owns writing the queued frame.
    void reprioritize(http_priority priority) &;
    void reprioritize(http_priority) && = delete;
    [[nodiscard]] std::span<const http_header> headers() const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::span<const http_header> trailers() const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const http_header> trailers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view) const&& = delete;
    [[nodiscard]] http_client_response_body& stream() & noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    http_client_response_body& stream() && = delete;
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) & {
        return body_.read(std::move(operation));
    }
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<http_client_response_bytes> bytes(std::size_t max_bytes = default_max_buffered_body_bytes, operation_options operation = {}) & {
        return body_.read_all(max_bytes, std::move(operation));
    }
    task<http_client_response_bytes> bytes(std::size_t = default_max_buffered_body_bytes, operation_options = {}) && = delete;
    [[nodiscard]] task<http_client_response_bytes> text(std::size_t max_bytes = default_max_buffered_body_bytes, operation_options operation = {}) & {
        return body_.read_all(max_bytes, std::move(operation));
    }
    task<http_client_response_bytes> text(std::size_t = default_max_buffered_body_bytes, operation_options = {}) && = delete;
    template <typename value_type>
        requires detail::is_model_json_value<value_type>
    [[nodiscard]] task<value_type> json(std::size_t max_bytes = default_max_buffered_body_bytes, operation_options operation = {}) & {
        return decode_json<value_type>(bytes(max_bytes, std::move(operation)));
    }
    template <typename value_type>
        requires detail::is_model_json_value<value_type>
    task<value_type> json(std::size_t = default_max_buffered_body_bytes, operation_options = {}) && = delete;
    // Forwards status, end-to-end fields, unread body and trailers. The cold
    // operation expires with either this response or the destination. Hop-by-hop
    // fields, those the Connection field names, and Content-Length are not
    // forwarded (RFC 9110 §7.6.1). operation bounds the whole forward, as
    // pipe_to(response_stream_writer&). Both destinations forward identically.
    [[nodiscard]] task<void> pipe_to(context& output, operation_options operation = {}) &;
    task<void> pipe_to(context&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> pipe_to(request_stream& output, operation_options operation = {}) &;
    task<void> pipe_to(request_stream&, operation_options = {}) && = delete;

private:
    // The one forwarding chain of both pipe_to() destinations.
    [[nodiscard]] task<void> pipe_to_destination(context_response_editor draft, response_stream_writer& writer,
        ::ruvia::operation_scope& destination, operation_options operation);
    template <typename value_type>
    static task<value_type> decode_json(task<http_client_response_bytes> input) {
        auto owned = co_await std::move(input);
        co_return from_json<value_type>(owned.text());
    }
    friend class detail::http_client_pool;
    friend class detail::http_client_websocket_transport;
    friend class http_client_exchange;
    friend class http_client_tunnel;
    friend class http_capsule_stream;
    friend class detail::http_capsule_stream_state;
    friend class detail::http_client_response_handoff;
    friend class http_client_push;
    friend class http_client_request_body_writer;
    friend class http_client_request;

    explicit http_client_response(detail::http_client_pool& pool);
    explicit http_client_response(detail::http_client_response_state* state_value) noexcept;
    void release() noexcept;

    detail::http_client_response_state* state_{nullptr};
    http_client_response_body body_{state_};
    bool consumer_{true};
};

// Owns an established CONNECT byte stream on its client worker. Metadata borrows
// this owner; a read view lasts until the next read starts. Inputs are copied before a
// cold write is returned. One running read and one running write/finish may coexist; cold operations do not reserve either lane. Moving the
// owner preserves operations. Destruction aborts both directions; shutdown joins
// transport drivers. finish() closes only the local sending direction and is
// idempotent after successful completion, including normal transport retirement.
// Each read/text/write/finish takes its own operation_options, timed from when
// it starts. A read's expiry or stop request ends only that read with
// http_client_error (errc::timed_out / operation_canceled) and keeps received bytes; a write's
// or finish's aborts the tunnel, since partly sent bytes cannot be withdrawn,
// and an already-stopped token fails before anything is queued.
class http_client_tunnel final {
public:
    http_client_tunnel(const http_client_tunnel&) = delete;
    http_client_tunnel& operator=(const http_client_tunnel&) = delete;
    http_client_tunnel(http_client_tunnel&& other) noexcept;
    http_client_tunnel& operator=(http_client_tunnel&& other) noexcept;
    ~http_client_tunnel();
    [[nodiscard]] http_status_code status() const {
        return response_.status();
    }
    [[nodiscard]] http_protocol_version protocol_version() const {
        return response_.protocol_version();
    }
    [[nodiscard]] http_client_response_framing framing() const {
        return response_.framing();
    }
    [[nodiscard]] std::optional<std::uint64_t> content_length() const {
        return response_.content_length();
    }
    [[nodiscard]] const http_client_connection_info& connection_info() const& RUVIA_LIFETIMEBOUND {
        return response_.connection_info();
    }
    const http_client_connection_info& connection_info() const&& = delete;
    [[nodiscard]] std::span<const http_header> headers() const& RUVIA_LIFETIMEBOUND {
        return response_.headers();
    }
    std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& RUVIA_LIFETIMEBOUND {
        return response_.header(name);
    }
    std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::span<const http_client_informational_response> informational_responses() const& RUVIA_LIFETIMEBOUND {
        return response_.informational_responses();
    }
    std::span<const http_client_informational_response> informational_responses() const&& = delete;
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<std::optional<std::string_view>> text(operation_options operation = {}) &;
    task<std::optional<std::string_view>> text(operation_options = {}) && = delete;
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
    // A successful finish stays idempotent after transport retirement.
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    // Transfers tunnel ownership to the Capsule Protocol adapter.
    [[nodiscard]] http_capsule_stream capsules(http_capsule_config config = {}) &&;
    http_capsule_stream capsules(http_capsule_config = {}) & = delete;
    // Checked ownership transfers for accepted, negotiated datagram/CONNECT-UDP
    // tunnels. Ordinary CONNECT throws std::logic_error without consuming this
    // owner. The Capsule Protocol limits apply directly to encoded datagrams.
    [[nodiscard]] http_datagram_stream datagrams(http_capsule_config config = {}) &&;
    http_datagram_stream datagrams(http_capsule_config = {}) & = delete;
    [[nodiscard]] http_udp_tunnel udp(http_capsule_config config = {}) &&;
    http_udp_tunnel udp(http_capsule_config = {}) & = delete;

private:
    friend class detail::http_client_pool;
    friend class detail::http_client_websocket_transport;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    explicit http_client_tunnel(http_client_response response) noexcept;
    [[nodiscard]] task<void> write_owned(std::string&& bytes, operation_options operation = {});
    // abort() for received content that violates the tunnel's protocol (RFC 9297
    // section 3.3 capsule error): HTTP/2 resets the stream with PROTOCOL_ERROR.
    void abort_malformed() & noexcept;
    void release() noexcept;
    http_client_response response_;
};

// A rejected CONNECT keeps its ordinary HTTP status, headers and response body.
// Only a successful handshake exposes a tunnel.
class http_client_tunnel_result final {
public:
    http_client_tunnel_result(const http_client_tunnel_result&) = delete;
    http_client_tunnel_result& operator=(const http_client_tunnel_result&) = delete;
    http_client_tunnel_result(http_client_tunnel_result&&) noexcept = default;
    http_client_tunnel_result& operator=(http_client_tunnel_result&&) noexcept = default;
    [[nodiscard]] http_client_tunnel* tunnel() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_client_tunnel>(&value_);
    }
    http_client_tunnel* tunnel() && = delete;
    [[nodiscard]] http_client_response* response() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_client_response>(&value_);
    }
    http_client_response* response() && = delete;

private:
    friend class detail::http_client_pool;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    friend class http_datagram_stream;
    explicit http_client_tunnel_result(http_client_tunnel tunnel) noexcept
        : value_(std::move(tunnel)) {}
    explicit http_client_tunnel_result(http_client_response response) noexcept
        : value_(std::move(response)) {}
    std::variant<http_client_tunnel, http_client_response> value_;
};

// A worker-affine, independently owned observation. Its metadata survives
// client shutdown and later observations, and expires when this owner dies.
// Destroy before the event_loop retires. A slot identifies the pool position
// of the observing connection owner, not a persistent connection: positions
// are reused once their owner is reclaimed.
class http_client_advertisement final {
public:
    http_client_advertisement(http_client_advertisement&& other) noexcept;
    http_client_advertisement& operator=(http_client_advertisement&& other) noexcept;
    http_client_advertisement(const http_client_advertisement&) = delete;
    http_client_advertisement& operator=(const http_client_advertisement&) = delete;
    ~http_client_advertisement();

    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    [[nodiscard]] std::size_t connection_slot() const noexcept;
    [[nodiscard]] const http_origin_advertisement* origins() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_origin_advertisement* origins() const&& = delete;
    [[nodiscard]] const http_alternative_service_advertisement* alternative_service() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_alternative_service_advertisement* alternative_service() const&& = delete;

private:
    friend class detail::http_client_advertisement_queue;
    explicit http_client_advertisement(detail::http_client_advertisement_state* state_value) noexcept
        : state_(state_value) {}
    detail::http_client_advertisement_state* state_{};
};

// An accepted promise owns its copied request and response storage on the
// client worker. Destruction cancels unfinished push. Like an ordinary response,
// it can survive client shutdown but must retire before the event_loop.
class http_client_push final {
public:
    http_client_push(const http_client_push&) = delete;
    http_client_push& operator=(const http_client_push&) = delete;
    http_client_push(http_client_push&&) noexcept;
    http_client_push& operator=(http_client_push&&) noexcept;
    ~http_client_push();
    [[nodiscard]] const http_push_request& request() const& RUVIA_LIFETIMEBOUND;
    const http_push_request& request() const&& = delete;
    // Transfers the response once, when the operation is started. Promise
    // metadata remains available through this push until it is destroyed.
    // An operation timeout or stop request ends only this wait for the head
    // with http_client_error (errc::timed_out / operation_canceled); a later response() retries.
    [[nodiscard]] task<http_client_response> response(operation_options operation = {}) &;
    task<http_client_response> response(operation_options = {}) && = delete;

private:
    friend class detail::http_client_pool;
    explicit http_client_push(http_client_response response) noexcept;
    void release() noexcept;
    http_client_response response_;
};

// One upload lane embedded in its exchange. Each chunk/trailer input is owned
// before return; completing write() means transport accepted the entire chunk.
// Only one write/end operation may run at a time. Cold operations can coexist
// and claim the output lane when awaited. Chunk size is bounded by
// max_chunk_bytes_; split larger sources into sequential writes. An HTTP/1
// transport write failure does not discard a complete final response arriving
// from the peer: response() remains consumable, while the failed upload is
// reported independently and its connection is not reused.
// A write()/end() timeout or stop request in its operation_options aborts the
// exchange with http_client_error (errc::timed_out / operation_canceled), since a partly sent
// chunk cannot be withdrawn; an already-stopped token fails before queuing.
class http_client_request_body_writer final {
public:
    http_client_request_body_writer(const http_client_request_body_writer&) = delete;
    http_client_request_body_writer& operator=(const http_client_request_body_writer&) = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::string_view bytes, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& bytes, operation_options operation = {}) & {
        return write_owned(std::move(bytes), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> end(std::span<const http_header_view> trailers = {}, operation_options operation = {}) &;
    task<void> end(std::span<const http_header_view> = {}, operation_options = {}) && = delete;
    [[nodiscard]] bool complete() const noexcept;
    // Whether write() and end() may still start: false once the content ended
    // or end() started, after abandon(), and once the exchange stopped the
    // upload itself (an early final response that completes the exchange, or a
    // failure). A false result never turns a later response into a failure.
    [[nodiscard]] bool writable() const noexcept;
    // Stops sending without failing the exchange: the request content stays
    // incomplete and response() remains consumable. HTTP/1 closes the
    // connection after the response; HTTP/2 and HTTP/3 end only the sending
    // direction (RFC 9113 section 8.1, RFC 9114 section 4.1). A running write()
    // or end() completes with errc::operation_canceled; idempotent, no-op once complete().
    void abandon() & noexcept;
    void abandon() && = delete;

private:
    friend class http_client_exchange;
    friend class http_client_request;
    explicit http_client_request_body_writer(detail::http_client_response_state* state_value) noexcept
        : state_(state_value) {}
    ~http_client_request_body_writer() = default;
    [[nodiscard]] task<void> write_owned(std::string&& bytes, operation_options operation);
    detail::http_client_response_state* state_;
};

// Owns a streaming request and its eventual response on one worker. Moving
// preserves operations because their state is address stable. Destruction
// abandons unfinished upload; client shutdown wakes and joins its producer.
class http_client_exchange final {
public:
    http_client_exchange(const http_client_exchange&) = delete;
    http_client_exchange& operator=(const http_client_exchange&) = delete;
    http_client_exchange(http_client_exchange&& other) noexcept;
    http_client_exchange& operator=(http_client_exchange&& other) noexcept;
    ~http_client_exchange();
    [[nodiscard]] http_client_request_body_writer& body() & noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    http_client_request_body_writer& body() && = delete;
    // Waits for the final head. An operation timeout or stop request ends only
    // this wait with http_client_error (errc::timed_out / operation_canceled); a later
    // response() retries, and the upload lane is unaffected.
    [[nodiscard]] task<http_client_response> response(operation_options operation = {}) &;
    task<http_client_response> response(operation_options = {}) && = delete;

private:
    friend class detail::http_client_pool;
    explicit http_client_exchange(http_client_response response) noexcept;
    void release() noexcept;
    http_client_response response_;
    http_client_request_body_writer body_;
};

class http_client_ref final {
public:
    http_client_ref(const http_client_ref& other);
    http_client_ref& operator=(const http_client_ref&) = delete;

    // Inherits an empty token; a supplied state replaces user cancellation.
    // Timeout can only tighten. Accumulation requires a caller-owned stop_group.
    // Request-host cancellation remains independent and cannot be replaced.
    [[nodiscard]] http_client_ref with_options(operation_options options) const;
    [[nodiscard]] http_client_request request(std::string_view method, std::string_view path) const;
    [[nodiscard]] http_client_request get(std::string_view path) const;
    [[nodiscard]] http_client_request post(std::string_view path) const;
    [[nodiscard]] http_client_request put(std::string_view path) const;
    [[nodiscard]] http_client_request patch(std::string_view path) const;
    [[nodiscard]] http_client_request del(std::string_view path) const;
    [[nodiscard]] http_client_request head(std::string_view path) const;
    [[nodiscard]] http_client_request options(std::string_view path) const;
    [[nodiscard]] http_client_connect_request connect(std::string_view authority) const;
    [[nodiscard]] http_client_connect_request udp(std::string_view path) const;
    [[nodiscard]] http_client_websocket_request websocket(std::string_view path) const;
    // Migrates existing authenticated HTTP/3 sessions only, without reconnect or
    // replay. Cancellation and the tighter of operation/connect timeout cover
    // handshake confirmation, CID availability and path validation together.
    [[nodiscard]] task<void> migrate(std::string_view local_address, std::uint16_t port = 0) const;
    [[nodiscard]] http_client_stats stats() const;
    [[nodiscard]] std::vector<http_client_endpoint_address> dns_snapshot() const;
    void update_dns(std::span<const http_client_endpoint_address> addresses) const;
    [[nodiscard]] task<void> refresh_dns() const;
    // Non-destructive DNS resolution: resolves the client's dial host (or a
    // specified host) into a sorted, deduplicated IP address list without
    // retiring existing active connections or WebSockets.
    [[nodiscard]] task<std::vector<ip_address>> resolve_dns(
        operation_options options = {}) const;
    [[nodiscard]] task<std::vector<ip_address>> resolve_dns(
        std::string_view host, operation_options options = {}) const;
    [[nodiscard]] bool close_connection(std::uint64_t id) const;
    [[nodiscard]] std::optional<http_client_advertisement> next_advertisement() const;
    [[nodiscard]] std::optional<http_client_push> next_push() const;
    [[nodiscard]] std::string_view host() const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] http_scheme scheme() const;
    [[nodiscard]] http_origin_view origin() const& RUVIA_LIFETIMEBOUND;
    http_origin_view origin() const&& = delete;

private:
    friend class http_client_request;
    friend class http_client_connect_request;
    friend class http_client_websocket_request;
    [[nodiscard]] task<http_client_response> dispatch(const http_client_request_view& request, const http_client_request_options& options,
        const std::shared_ptr<http_client_informational_callback>& informational) const;
    [[nodiscard]] task<http_client_exchange> dispatch_upload(const http_client_request_view& head, http_client_upload_config upload, const http_client_request_options& options,
        const std::shared_ptr<http_client_informational_callback>& informational) const;
    [[nodiscard]] task<http_client_tunnel_result> dispatch_connect(const http_client_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& options,
        const std::shared_ptr<http_client_informational_callback>& informational) const;
    [[nodiscard]] task<http_client_tunnel_result> dispatch_udp(const http_client_udp_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& options,
        const std::shared_ptr<http_client_informational_callback>& informational) const;
    friend class detail::http_client_registry;
    friend class detail::http_client_state;
    friend class context;
    friend class web_worker_context;
    friend class detail::worker_client_registry_view;
    http_client_ref(detail::http_client_pool& pool, ::ruvia::operation_scope& scope) noexcept;
    http_client_ref(detail::http_client_pool& pool, ::ruvia::operation_scope& scope, operation_options options) noexcept;
    [[nodiscard]] http_client_ref bind_caller_scope(
        ::ruvia::operation_scope& scope, const stop_token& host_stop) && noexcept;
    static void expire_capability(void* target) noexcept;

    detail::http_client_pool* pool_{nullptr};
    std::shared_ptr<detail::http_client_pool> pool_owner_;
    operation_options options_;
    stop_token host_stop_token_;
    scoped_capability_registration registration_;
    const detail::http_client_registry* registry_{};
};

// Owns request inputs before any cold operation is returned. Each send/upload
// consumes this builder; response bodies are one linear stream, not replayable.
class http_client_request final {
public:
    http_client_request(const http_client_request&) = delete;
    http_client_request& operator=(const http_client_request&) = delete;
    http_client_request(http_client_request&&) = default;
    http_client_request& operator=(http_client_request&&) = delete;
    http_client_request& header(std::string_view name, std::string_view value);
    http_client_request& headers(std::span<const http_header_view> fields);
    http_client_request& query(std::string_view name, std::string_view value);
    http_client_request& body(std::string_view bytes);
    http_client_request& body(std::span<const std::byte> bytes);
    // Replaces every per-request option, including the informational callback.
    http_client_request& configure(http_client_request_options options);
    // Per-hop redirect observation and control callback invoked on every
    // redirect hop (same-origin and cross-origin alike) before following it.
    // Returning false stops following and returns the redirect response.
    http_client_request& on_redirect(http_client_redirect_observer observer);
    http_client_request& on_redirect(http_client_async_redirect_observer observer);
    template <typename value_type>
        requires detail::is_model_json_value<value_type>
    http_client_request& json(const value_type& value) {
        if (consumed_) {
            throw std::logic_error("HTTP request builder was consumed");
        }
        auto encoded = to_json(value);
        header("Content-Type", "application/json");
        body_.swap(encoded);
        has_body_ = true;
        return *this;
    }
    http_client_request& redirects(std::size_t maximum);
    [[nodiscard]] task<http_client_response> send();
    [[nodiscard]] task<http_client_exchange> upload(http_client_upload_config config = {});
    [[nodiscard]] task<http_client_tunnel_result> upgrade(http_client_tunnel_config config = {});

private:
    friend class http_client_ref;
    friend class detail::http_client_response_handoff;
    http_client_request(const http_client_ref& client, std::string_view method, std::string_view path);
    static task<http_client_response> send_owned(http_client_request request);
    static task<http_client_exchange> upload_owned(
        http_client_request request, http_client_upload_config config);
    static task<http_client_response> follow_redirects(http_client_request request,
        std::optional<http_client_response> initial_response, const operation_deadline& total,
        bool upload_initial_hop);
    [[nodiscard]] std::vector<http_header_view> field_views() const;
    http_client_ref client_;
    std::string method_;
    std::string target_;
    std::vector<http_header> headers_;
    std::string body_;
    std::size_t max_redirects_{5};
    // informational_ is moved out into informational_callback_, which every
    // redirect hop's exchange shares.
    http_client_request_options request_options_;
    std::shared_ptr<http_client_informational_callback> informational_callback_;
    http_client_redirect_observer redirect_observer_;
    http_client_async_redirect_observer async_redirect_observer_;
    bool has_body_{false};
    bool consumed_{false};
};

class http_client_connect_request final {
public:
    http_client_connect_request(const http_client_connect_request&) = delete;
    http_client_connect_request& operator=(const http_client_connect_request&) = delete;
    http_client_connect_request(http_client_connect_request&&) = default;
    http_client_connect_request& operator=(http_client_connect_request&&) = delete;
    http_client_connect_request& header(std::string_view name, std::string_view value);
    http_client_connect_request& headers(std::span<const http_header_view> fields);
    http_client_connect_request& configure(http_client_request_options options);
    http_client_connect_request& protocol(std::string_view value);
    http_client_connect_request& target(std::string_view value);
    [[nodiscard]] task<http_client_tunnel_result> open(http_client_tunnel_config config = {});

private:
    friend class http_client_ref;
    http_client_connect_request(const http_client_ref& client, std::string_view authority, std::string_view target, bool udp);
    http_client_ref client_;
    std::string authority_;
    std::string protocol_;
    std::string target_;
    std::vector<http_header> headers_;
    http_client_request_options request_options_;
    std::shared_ptr<http_client_informational_callback> informational_callback_;
    bool udp_;
    bool consumed_{false};
};

// 绑定一个 Ruvia event loop 的出站 origin 集合，可在所属 worker 动态更新。
// 构造不创建线程；send() 按需建连。
class http_client final {
public:
    http_client(event_loop loop, const http_client_config& config,
        const http_client_worker_config& worker = {});
    http_client(event_loop loop, std::span<const http_client_config> configs,
        const http_client_worker_config& worker = {});
    ~http_client();

    http_client(const http_client&) = delete;
    http_client& operator=(const http_client&) = delete;
    http_client(http_client&&) = delete;
    http_client& operator=(http_client&&) = delete;

    // Handles and operations borrow this client's open lifecycle. Starting one
    // from a temporary would immediately run the client destructor and begin
    // shutdown, so only lvalue clients may create them. Like context::client(),
    // a handle carries no operation options; derive them with with_options().
    // client() selects the sole configuration, or alias "default" in a set.
    // 默认重定向仅可到达已注册 origin；请求策略可显式扩充集合。
    [[nodiscard]] http_client_ref client() const&;
    http_client_ref client() const&& = delete;
    [[nodiscard]] http_client_ref client(std::string_view alias) const&;
    http_client_ref client(std::string_view) const&& = delete;
    // Worker-local 发布；调用时拥有配置，await 在 blocking_pool 准备 TLS 后原子替换。
    // 无 TLS 文件准备时无需 blocking_pool；取消或准备失败不改变既有 alias。
    // 旧池取消并 join，最后一个旧句柄/冷操作释放后回收；其他 alias 不变。
    [[nodiscard]] task<void> upsert(const http_client_config& config, operation_options options = {}) &;
    task<void> upsert(const http_client_config&, operation_options = {}) && = delete;
    [[nodiscard]] bool remove(std::string_view alias) &;
    bool remove(std::string_view) && = delete;

    // Idempotent and callable from any thread. It only requests immediate
    // shutdown; use shutdown() when the worker teardown must be awaited.
    void close() noexcept;
    // Cancels and joins client producers/send operations, then retires their
    // transport borrows on the bound loop. Response body consumers are owned by
    // the response, not this shutdown: finish/join them and destroy responses
    // on the owning worker before the event_loop retires.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    std::shared_ptr<detail::http_client_state> state_;
};

// Standalone non-destructive asynchronous DNS resolution bound to a worker or
// event loop. Returns a sorted, deduplicated list of numeric IP addresses.
[[nodiscard]] task<std::vector<ip_address>> resolve_dns(
    const worker_ref& worker, std::string_view host, operation_options options = {});
[[nodiscard]] task<std::vector<ip_address>> resolve_dns(
    event_loop loop, std::string_view host, operation_options options = {});

}  // namespace ruvia

namespace ruvia {

// Every input is owned before connect() returns its cold operation. The builder
// is consumed once; headers remain application fields, never handshake fields.
class http_client_websocket_request final {
public:
    http_client_websocket_request(const http_client_websocket_request&) = delete;
    http_client_websocket_request& operator=(const http_client_websocket_request&) = delete;
    http_client_websocket_request(http_client_websocket_request&&) = default;
    http_client_websocket_request& operator=(http_client_websocket_request&&) = delete;
    http_client_websocket_request& headers(std::span<const http_header_view> fields);
    http_client_websocket_request& header(std::string_view name, std::string_view value);
    http_client_websocket_request& configure(http_client_request_options options);
    http_client_websocket_request& subprotocol(std::string_view value);
    http_client_websocket_request& deflate(websocket_client_deflate_offer offer);
    // Pong expiry runs only while receive can observe it; blocked writes retain
    // their independent liveness bound. An absent ping interval disables probing.
    http_client_websocket_request& lifecycle(websocket_lifecycle_options options);
    // Applies to incoming messages and submitted application frames; zero is invalid.
    http_client_websocket_request& max_message_bytes(std::size_t limit);
    // Connect and request deadlines bound opening only, not the session lifetime.
    [[nodiscard]] task<websocket> connect();

private:
    friend class http_client_ref;
    http_client_websocket_request(const http_client_ref& client, std::string_view path);
    [[nodiscard]] static task<websocket> connect_owned(http_client_websocket_request request);
    void require_open() const;
    http_client_ref client_;
    std::string path_;
    std::vector<http_header> headers_;
    std::vector<std::string> subprotocols_;
    websocket_client_deflate_offer deflate_{};
    websocket_lifecycle_options lifecycle_{};
    std::size_t max_message_bytes_{default_max_websocket_message_bytes};
    http_client_request_options request_options_;
    bool consumed_{false};
};

}  // namespace ruvia
