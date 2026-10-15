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
// A final response that closes HTTP/1 is followed by send-side shutdown (TLS:
// close_notify) and TCP close, without draining peer input. Unread or late peer
// data can therefore cause a reset that prevents receipt of the final response
// (RFC 9112 §9.6); the server does not wait for the peer's FIN.
struct server_config final {
    // Business workers; one server network thread is additional.
    std::size_t worker_count_{(std::max)(1U, std::thread::hardware_concurrency())};
    process_signal_handler_policy process_signal_handlers_{process_signal_handler_policy::external_owner};
    // Graceful stop of a serving app: once stop() closes admission, admitted
    // requests, tunnels and WebSocket close handshakes may finish for up to
    // this long before every remaining connection is closed. Zero closes them
    // at once.
    std::chrono::milliseconds drain_timeout_{std::chrono::seconds(10)};
    std::size_t worker_queue_capacity_{1024};
    // HTTP connection inactivity: the keep-alive wait for the next request,
    // and a handler -- buffered or response-stream -- that neither reads nor
    // writes the connection (for a stream, the wait between two writes).
    // Upgraded WebSockets use their route lifecycle heartbeat and
    // close-handshake deadlines instead, even when heartbeat is disabled.
    std::optional<std::chrono::milliseconds> idle_timeout_{std::chrono::seconds(75)};
    // How often connection timeouts are checked. A timeout fires once at least
    // its duration has passed, and at most about two intervals later.
    std::chrono::milliseconds connection_scan_interval_{std::chrono::seconds(1)};
    std::optional<std::chrono::milliseconds> request_header_timeout_{std::chrono::seconds(60)};
    std::optional<std::chrono::milliseconds> request_body_timeout_{std::chrono::seconds(60)};
    // Absolute phase deadlines: progress does not renew these limits.
    std::optional<std::chrono::milliseconds> header_completion_timeout_{std::chrono::seconds(30)};
    std::optional<std::chrono::milliseconds> body_completion_timeout_{std::chrono::seconds(120)};
    // Inactivity while a response (a buffered response, or one write of a
    // response stream) is being written. Successful transport writes renew
    // this limit; it is not a deadline for completing the whole response.
    // Time between response-stream writes is governed by idle_timeout_.
    std::optional<std::chrono::milliseconds> write_timeout_{std::chrono::seconds(60)};
    // Bounded by default so a connection flood cannot exhaust FDs or memory.
    // Excess accepted sockets are closed before TLS or HTTP protocol
    // detection, so admission never fabricates a response.
    std::optional<std::size_t> max_connections_per_worker_{1024};
    // Cumulative request cap for each connection.
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
    // Each worker's memory. retained_block_bytes_ bounds the freed 16 KiB-1 MiB
    // blocks a worker keeps for reuse. A worker returns them, with its cached
    // coroutine frames, at the first connection scan that follows a whole
    // connection_scan_interval_ with no request exchange in progress and no
    // connection activity.
    memory_pool_config memory_pool_{};
    // Shared by all registered outbound HTTP client aliases on each worker.
    http_client_result_budget_config http_client_result_budget_{};
};

struct trusted_proxy_config final {
    // CIDR lengths count bits in the literal's address family. For example,
    // 127.0.0.0/8 and ::ffff:127.0.0.0/104 describe the same mapped network.
    std::vector<std::string> cidrs_{};
    // Enable only when every trusted proxy sanitizes and appends matching
    // X-Forwarded-For / X-Forwarded-Proto elements.
    bool trust_x_forwarded_proto_{false};
};

namespace detail {
struct access_log_record_access;
struct connection_failure_record_access;
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
    std::vector<tls_sni_config> sni_{};
};

// One bind address with optional HTTP and HTTPS service ports, independent of
// the wire protocol. application validates and expands this value atomically. An
// omitted port disables that service. auto_https_redirect_ targets the HTTPS
// service port.
struct listen_config final {
    // Numeric IPv4 or IPv6 bind address, normalized when application::listen consumes
    // this configuration.
    std::string address_{"0.0.0.0"};
    std::optional<std::uint16_t> http_{};
    std::optional<std::uint16_t> https_{};
    tls_config tls_{};
    bool auto_https_redirect_{false};
};

// Canonical startup values shared by application configuration and every worker's
// server options. They stay top-level so configuration is not copied between
// models.
struct compression_config final {
    // Buffered in-memory responses below this size remain identity. File
    // responses (context::file, context::static_file) are never encoded at
    // request time, whether their bytes are read from disk or memory, so GET
    // and HEAD describe the same representation.
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

    [[nodiscard]] constexpr std::string_view remote_address() const noexcept {
        return remote_address_;
    }

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr std::uint64_t duration_micros() const noexcept {
        return duration_micros_;
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
        std::string_view remote_address_;
        http_status_code status_;
        std::uint64_t duration_micros_;
    };

    explicit constexpr access_log_record(const fields_type& fields) noexcept
        : method_(fields.method_),
          path_(fields.path_),
          remote_address_(fields.remote_address_),
          duration_micros_(fields.duration_micros_),
          status_(fields.status_),
          known_method_(fields.known_method_),
          protocol_version_(fields.protocol_version_) {}

    std::string_view method_;
    std::string_view path_;
    std::string_view remote_address_;
    std::uint64_t duration_micros_;
    http_status_code status_;
    http_known_method known_method_;
    http_protocol_version protocol_version_;
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
    // Connections held right now, against server_config::max_connections_per_worker_.
    std::size_t active_connections_{0};
    // Connections closed on accept because that budget was full. A rising
    // count means the server is shedding load rather than queueing it.
    std::size_t connections_refused_{0};
    // Connections lost to an exception, as delivered to on_connection_failure
    // (peer resets and closes and server-cancelled I/O are not failures).
    std::size_t connection_failures_{0};
    // Accepts that failed transiently (descriptor exhaustion, a session that
    // could not be started). Each cost one connection, not the listener.
    std::size_t accept_failures_{0};
    // Failures that escaped to the worker's io_context and stopped it.
    std::size_t worker_failures_{0};
    // Document-root refresh passes whose replacement index could not be built
    // or adopted. The application runs one refresher however many workers it
    // has, so each failed pass counts once. The previous complete snapshot
    // remains active; this counter makes filesystem/permission failures
    // observable without taking a worker down.
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
