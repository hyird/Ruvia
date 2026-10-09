#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/memory_pool_config.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/detail/callback.h"
#include "ruvia/web/detail/callback_ref.h"
#include "ruvia/web/http3_qpack_config.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/static_files.h"

namespace ruvia {

enum class process_signal_handler_policy : std::uint8_t {
    external_owner,
    install,
};

// The complete always-present Web server runtime configuration. application consumes
// this aggregate atomically, validates it as one unit, and publishes the same
// normalized values to every worker. Optional fields disable only the policy
// they name; optional application capabilities remain separate config-or-null calls.
struct server_config final {
    // Business workers; one server network thread is additional.
    std::size_t worker_count_{(std::max)(1U, std::thread::hardware_concurrency())};
    process_signal_handler_policy process_signal_handlers_{process_signal_handler_policy::external_owner};
    std::size_t worker_queue_capacity_{1024};
    // HTTP connection inactivity; upgraded WebSockets use their route lifecycle
    // heartbeat and close-handshake deadlines instead, even when heartbeat is disabled.
    std::optional<std::chrono::milliseconds> idle_timeout_{std::chrono::seconds(75)};
    std::chrono::milliseconds connection_scan_interval_{std::chrono::seconds(1)};
    std::optional<std::chrono::milliseconds> request_header_timeout_{std::chrono::seconds(60)};
    std::optional<std::chrono::milliseconds> request_body_timeout_{std::chrono::seconds(60)};
    // Absolute phase deadlines: progress does not renew these limits.
    std::optional<std::chrono::milliseconds> header_completion_timeout_{std::chrono::seconds(30)};
    std::optional<std::chrono::milliseconds> body_completion_timeout_{std::chrono::seconds(120)};
    std::optional<std::chrono::milliseconds> write_timeout_{std::chrono::seconds(60)};
    // HTTP/3 requires a finite per-worker connection cap.
    std::optional<std::size_t> max_connections_per_worker_{1024};
    // Cumulative request cap per HTTP/1, HTTP/2, or HTTP/3 connection.
    // HTTP/3 uses fixed cutoff 4*N. A request stream ID at/above it, arriving
    // before N admitted requests, triggers early GOAWAY; such streams are
    // rejected individually. Undecided lower-ID requests remain admissible
    // until N, then admission seals/drains.
    // Default 1000; not silently clamped.
    std::optional<std::size_t> max_requests_per_connection_{1000};
    std::size_t max_buffered_body_bytes_{default_max_buffered_body_bytes};
    // Live inbound buffer allocations across HTTP bodies and websocket sessions.
    // Includes capacity and reallocation overlap; the limits cannot be disabled.
    std::size_t max_inbound_buffer_bytes_per_worker_{256 * 1024 * 1024};
    std::size_t max_inbound_buffer_bytes_per_connection_{64 * 1024 * 1024};
    std::optional<std::size_t> max_stream_body_bytes_{};
    std::size_t max_websocket_message_bytes_{default_max_websocket_message_bytes};
    memory_pool_config memory_pool_{};
    // Shared by all registered outbound HTTP client aliases on each worker.
    http_client_result_budget_config http_client_result_budget_{};
};

struct trusted_proxy_config final {
    std::vector<std::string> cidrs_{};
    // Enable only when every trusted proxy sanitizes and appends matching
    // X-Forwarded-For / X-Forwarded-Proto elements.
    bool trust_x_forwarded_proto_{false};
};

namespace detail {
struct access_log_record_access;
struct access_log_sink;
struct connection_failure_record_access;
struct connection_failure_sink;
}  // namespace detail

enum class tls_client_certificate_requirement : std::uint8_t {
    optional,
    required,
};

struct tls_client_certificate_config final {
    // A CA bundle used to verify presented client certificates. Optional mode
    // admits a client without a certificate; required mode rejects it.
    std::optional<std::filesystem::path> verify_file_{};
    tls_client_certificate_requirement requirement_{tls_client_certificate_requirement::optional};
};

struct tls_sni_config final {
    std::string host_{};
    std::filesystem::path certificate_chain_file_{};
    std::filesystem::path private_key_file_{};
    std::string private_key_password_{};
};

struct tls_config final {
    std::filesystem::path certificate_chain_file_{};
    std::filesystem::path private_key_file_{};
    std::string private_key_password_{};
    tls_client_certificate_config client_certificates_{};
    // Disabled by default. Early requests still require an explicit replay-safe
    // route and remain subject to OpenSSL's built-in anti-replay protection.
    bool http3_early_data_{false};
    std::vector<tls_sni_config> sni_{};
};

enum class http3_mode : std::uint8_t {
    automatic,
    enabled,
    disabled,
};

// HTTP/3 is an optional UDP capability on the HTTPS listener. Automatic mode
// enables QUIC when HTTPS is configured; it never changes TCP ALPN.
struct http3_listen_config final {
    http3_mode mode_{http3_mode::automatic};
    http3_qpack_config qpack_{};
    // Per-worker stream DATA, CONTROL and release slots, independent of task admission.
    std::size_t stream_buffer_capacity_{1024};
    // Acceptor-to-worker input slots and worker-to-Acceptor output credits.
    std::size_t datagram_input_capacity_{64};
    std::size_t datagram_output_capacity_{16};
    // Per-connection deadline to complete the QUIC/TLS handshake; default 10 seconds.
    std::chrono::milliseconds handshake_timeout_{std::chrono::seconds(10)};
    // Maximum time to drain admitted request streams and close the connection.
    std::chrono::milliseconds drain_timeout_{std::chrono::seconds(30)};
};

enum class alt_svc_mode : std::uint8_t {
    automatic,
    disabled,
    clear,
};

// HTTPS responses advertise an active HTTP/3 listener by default. Applications
// can still replace or remove Alt-Svc on an individual response. Clear mode
// emits the RFC 7838 cache-clearing value even when HTTP/3 is disabled.
struct alt_svc_config final {
    alt_svc_mode mode_{alt_svc_mode::automatic};
    std::chrono::seconds max_age_{std::chrono::hours(24)};
    bool persist_{false};
    // Defaults to the HTTPS/UDP listener port. This override is for deployments
    // whose externally advertised port differs from the local bind port.
    std::optional<std::uint16_t> advertised_port_{};
};

// One bind address with optional HTTP and HTTPS service ports, independent of
// the wire protocol. application validates and expands this value atomically. An
// omitted port disables that service. auto_https_redirect_ targets the HTTPS
// service port. HTTP/3 does not have a separate public listener port.
struct listen_config final {
    // Numeric IPv4 or IPv6 bind address, normalized when application::listen consumes
    // this configuration.
    std::string address_{"0.0.0.0"};
    std::optional<std::uint16_t> http_{};
    std::optional<std::uint16_t> https_{};
    tls_config tls_{};
    http3_listen_config http3_{};
    alt_svc_config alt_svc_{};
    bool auto_https_redirect_{false};
};

// Canonical startup values shared by application configuration and every worker's
// server options. They stay top-level so configuration is not copied between
// models.
struct compression_config final {
    // Buffered in-memory responses below this size remain identity.
    std::size_t min_bytes_{1024};
    // Eligible bodies through this size are encoded synchronously on the
    // worker; larger bodies are offloaded to the bounded blocking pool when it
    // is enabled, otherwise they are also encoded synchronously.
    std::size_t sync_bytes_{std::size_t{64} * 1024};
    // Buffered bodies above this size remain identity. Static files use their
    // own document-root precompression thresholds and still prefer checked-in
    // precompressed sidecars when present.
    std::size_t max_bytes_{std::size_t{64} * 1024 * 1024};
};

enum class cors_origin_mode : std::uint8_t {
    any,
    exact,
    credentialed_exact,
};

struct cors_origin_config final {
    cors_origin_mode mode_{cors_origin_mode::any};
    // Required for exact modes. "null" represents the serialized opaque
    // origin; wildcard mode requires this field to remain empty.
    std::string value_{};
};

enum class cors_request_headers_mode : std::uint8_t {
    reflect,
    fixed,
};

struct cors_request_headers_config final {
    cors_request_headers_mode mode_{cors_request_headers_mode::reflect};
    // Required in fixed mode and empty in reflect mode.
    std::vector<std::string> names_{};
};

struct cors_config final {
    cors_origin_config origin_{};
    cors_request_headers_config request_headers_{};
    std::vector<std::string> expose_headers_{};
    std::optional<std::chrono::seconds> max_age_{};
};

// How long a handler may run. The phase timeouts bound reading the head, reading
// the body and writing the response; this bounds the handler between them.
//
// When it elapses the request's stop token trips, so every wait that takes that
// token returns at once and the handler unwinds into an error response. It is
// cooperative -- see ruvia::Deadline for what that does and does not bound.
//
// A route may declare a shorter one with ruvia::Deadline<N>; it can never extend
// this. A struct rather than a bare duration so a future field can narrow the
// protocol scanner deadman that already covers a suspended handler down to
// something derived from this deadline, rather than arriving as a second setter
// with its own name.
struct deadline_config final {
    std::chrono::milliseconds handler_{0};
};

// Runtime behavior belongs to the server's document-root binding, not to the
// immutable static_root index. application document roots are always refreshed; a
// standalone static_root remains immutable because it has no server runtime.
struct document_root_runtime_config final {
    std::chrono::milliseconds refresh_interval_{std::chrono::seconds(1)};
};

struct document_root_config final {
    std::filesystem::path root_{};
    static_root_options static_options_{};
    document_root_runtime_config runtime_{};
    bool precompress_gzip_{false};
    bool precompress_brotli_{false};
    bool precompress_zstd_{false};
    std::size_t precompress_min_bytes_{1024};
    std::size_t precompress_max_bytes_{std::size_t{256} * 1024};
};

// One terminal response outcome with a committed final status, passed to the
// access-log callback after a complete buffered response head has reached the
// transport or a stream head is committed. The record borrows the immutable
// request and connection-owned remote address; the record and all returned views
// are valid only for the callback.
class access_log_record final {
public:
    [[nodiscard]] std::string_view method() const noexcept {
        return request_.method();
    }

    [[nodiscard]] http_known_method known_method() const noexcept {
        return request_.known_method();
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return request_.path();
    }

    [[nodiscard]] constexpr std::string_view remote_address() const noexcept {
        return remote_address_;
    }

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr std::uint64_t duration_micros() const noexcept {
        return duration_micros_;
    }

    [[nodiscard]] http_protocol_version protocol_version() const noexcept {
        return request_.protocol_version();
    }

private:
    friend struct detail::access_log_record_access;

    constexpr access_log_record(const http_request& request, std::string_view remote_address,
        http_status_code status, std::uint64_t duration_micros) noexcept
        : request_(request),
          remote_address_(remote_address),
          status_(status),
          duration_micros_(duration_micros) {}

    const http_request& request_;
    std::string_view remote_address_;
    http_status_code status_;
    std::uint64_t duration_micros_;
};

namespace detail {
using access_log_callback_ref_type = callback_ref_type<void(const access_log_record&) noexcept>;
}  // namespace detail

// application-owned access-log listener. Request dispatch receives only an internal,
// allocation-free callback_ref_type and never participates in this owner's lifetime.
using access_log_callback_type = detail::callback<void(const access_log_record&) noexcept>;

// One connection lost to an exception that escaped its session: a handler bug
// past the response's point of no return, an error handler that itself failed,
// or resource exhaustion. The server closes that connection and keeps serving;
// this record is how the failure becomes visible instead of vanishing with the
// connection. A request that fails before its response is committed never gets
// here -- it is answered with a 5xx through on_error.
//
// Views and the record itself are valid only for the callback invocation.
class connection_failure_record final {
public:
    // Empty when the peer address could not be read (the connection was
    // already gone) or when the failure happened before it was resolved.
    [[nodiscard]] constexpr std::string_view remote_address() const noexcept {
        return remote_address_;
    }

    // Never null. Rethrow it to inspect the failure.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

private:
    friend struct detail::connection_failure_record_access;

    connection_failure_record(std::string_view remote_address, std::exception_ptr exception) noexcept
        : remote_address_(remote_address),
          exception_(std::move(exception)) {}

    std::string_view remote_address_;
    std::exception_ptr exception_;
};

// HTTP serving counters, for health checks and metrics. Cumulative since the
// worker started and never reset, except active_connections_, which is a gauge.
// application::http_stats() sums these across every worker; each field is sampled
// independently, so treat them as a set of gauges rather than one snapshot.
//
// These make a server observable without installing any callback: on_error sees
// request failures and on_connection_failure sees lost connections, but neither
// answers "how many, since when".
struct http_server_stats final {
    // Connections held right now, against http_server_options::max_connections.
    std::size_t active_connections_{0};
    // Connections closed on accept because that budget was full. A rising
    // count means the server is shedding load rather than queueing it.
    std::size_t connections_refused_{0};
    // Connections lost to an exception, as delivered to on_connection_failure.
    std::size_t connection_failures_{0};
    // Accepts that failed transiently (descriptor exhaustion, a session that
    // could not be started). Each cost one connection, not the listener.
    std::size_t accept_failures_{0};
    // Failures that escaped to the worker's io_context and stopped it.
    std::size_t worker_failures_{0};
    // Polling refreshes whose replacement index could not be built. The
    // previous complete document-root snapshot remains active; this counter
    // makes filesystem/permission failures observable without taking the
    // worker down.
    std::size_t document_root_refresh_failures_{0};
};

namespace detail {
using connection_failure_callback_ref_type = callback_ref_type<void(const connection_failure_record&) noexcept>;
}  // namespace detail

// application-owned connection-failure listener. The listener must not throw: it runs on the last
// line of defense for a connection, where a second failure would have nowhere
// left to go, so the requirement is enforced at compile time rather than
// swallowed at runtime.
using connection_failure_callback_type = detail::callback<void(const connection_failure_record&) noexcept>;

}  // namespace ruvia
