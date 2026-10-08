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

#include "ruvia/core/memory/MemoryPoolConfig.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/web/Http3QpackConfig.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/Callback.h"
#include "ruvia/web/detail/CallbackRef.h"

namespace ruvia {

enum class process_signal_handler_policy : std::uint8_t {
    external_owner,
    install,
};

// The complete always-present Web server runtime configuration. App consumes
// this aggregate atomically, validates it as one unit, and publishes the same
// normalized values to every worker. Optional fields disable only the policy
// they name; optional App capabilities remain separate config-or-null calls.
struct server_config final {
    // Business workers; one server network thread is additional.
    std::size_t worker_count{(std::max)(1U, std::thread::hardware_concurrency())};
    process_signal_handler_policy process_signal_handlers{process_signal_handler_policy::external_owner};
    std::size_t worker_queue_capacity{1024};
    // HTTP connection inactivity; upgraded WebSockets use their route lifecycle
    // heartbeat and close-handshake deadlines instead, even when heartbeat is disabled.
    std::optional<std::chrono::milliseconds> idle_timeout{std::chrono::seconds(75)};
    std::chrono::milliseconds connection_scan_interval{std::chrono::seconds(1)};
    std::optional<std::chrono::milliseconds> request_header_timeout{std::chrono::seconds(60)};
    std::optional<std::chrono::milliseconds> request_body_timeout{std::chrono::seconds(60)};
    // Absolute phase deadlines: progress does not renew these limits.
    std::optional<std::chrono::milliseconds> header_completion_timeout{std::chrono::seconds(30)};
    std::optional<std::chrono::milliseconds> body_completion_timeout{std::chrono::seconds(120)};
    std::optional<std::chrono::milliseconds> write_timeout{std::chrono::seconds(60)};
    // HTTP/3 requires a finite per-worker connection cap.
    std::optional<std::size_t> max_connections_per_worker{1024};
    // Cumulative request cap per HTTP/1, HTTP/2, or HTTP/3 connection.
    // HTTP/3 uses fixed cutoff 4*N. A request stream ID at/above it, arriving
    // before N admitted requests, triggers early GOAWAY; such streams are
    // rejected individually. Undecided lower-ID requests remain admissible
    // until N, then admission seals/drains.
    // Default 1000; not silently clamped.
    std::optional<std::size_t> max_requests_per_connection{1000};
    std::size_t max_buffered_body_bytes{kDefaultMaxBufferedBodyBytes};
    // Live inbound buffer allocations across HTTP bodies and WebSocket sessions.
    // Includes capacity and reallocation overlap; the limits cannot be disabled.
    std::size_t max_inbound_buffer_bytes_per_worker{256 * 1024 * 1024};
    std::size_t max_inbound_buffer_bytes_per_connection{64 * 1024 * 1024};
    std::optional<std::size_t> max_stream_body_bytes{};
    std::size_t max_web_socket_message_bytes{kDefaultMaxWebSocketMessageBytes};
    MemoryPoolConfig memory_pool{};
    // Shared by all registered outbound HTTP client aliases on each worker.
    HttpClientResultBudgetConfig http_client_result_budget{};
};

struct TrustedProxyConfig final {
    std::vector<std::string> cidrs{};
    // Enable only when every trusted proxy sanitizes and appends matching
    // X-Forwarded-For / X-Forwarded-Proto elements.
    bool trust_x_forwarded_proto{false};
};

namespace detail {
struct AccessLogRecordAccess;
struct AccessLogSink;
struct ConnectionFailureRecordAccess;
struct ConnectionFailureSink;
}  // namespace detail

enum class TlsClientCertificateRequirement : std::uint8_t {
    kOptional,
    kRequired,
};

struct TlsClientCertificateConfig final {
    // A CA bundle used to verify presented client certificates. Optional mode
    // admits a client without a certificate; required mode rejects it.
    std::optional<std::filesystem::path> verifyFile{};
    TlsClientCertificateRequirement requirement{TlsClientCertificateRequirement::kOptional};
};

struct TlsSniConfig final {
    std::string host{};
    std::filesystem::path certificateChainFile{};
    std::filesystem::path privateKeyFile{};
    std::string privateKeyPassword{};
};

struct TlsConfig final {
    std::filesystem::path certificateChainFile{};
    std::filesystem::path privateKeyFile{};
    std::string privateKeyPassword{};
    TlsClientCertificateConfig clientCertificates{};
    // Disabled by default. Early requests still require an explicit replay-safe
    // route and remain subject to OpenSSL's built-in anti-replay protection.
    bool http3_early_data{false};
    std::vector<TlsSniConfig> sni{};
};

enum class Http3Mode : std::uint8_t {
    kAutomatic,
    kEnabled,
    kDisabled,
};

// HTTP/3 is an optional UDP capability on the HTTPS listener. Automatic mode
// enables QUIC when HTTPS is configured; it never changes TCP ALPN.
struct Http3ListenConfig final {
    Http3Mode mode{Http3Mode::kAutomatic};
    Http3QpackConfig qpack{};
    // Per-worker stream DATA, CONTROL and release slots, independent of task admission.
    std::size_t stream_buffer_capacity{1024};
    // Acceptor-to-worker input slots and worker-to-Acceptor output credits.
    std::size_t datagram_input_capacity{64};
    std::size_t datagram_output_capacity{16};
    // Per-connection deadline to complete the QUIC/TLS handshake; default 10 seconds.
    std::chrono::milliseconds handshakeTimeout{std::chrono::seconds(10)};
    // Maximum time to drain admitted request streams and close the connection.
    std::chrono::milliseconds drainTimeout{std::chrono::seconds(30)};
};

enum class AltSvcMode : std::uint8_t {
    kAutomatic,
    kDisabled,
    kClear,
};

// HTTPS responses advertise an active HTTP/3 listener by default. Applications
// can still replace or remove Alt-Svc on an individual response. Clear mode
// emits the RFC 7838 cache-clearing value even when HTTP/3 is disabled.
struct AltSvcConfig final {
    AltSvcMode mode{AltSvcMode::kAutomatic};
    std::chrono::seconds maxAge{std::chrono::hours(24)};
    bool persist{false};
    // Defaults to the HTTPS/UDP listener port. This override is for deployments
    // whose externally advertised port differs from the local bind port.
    std::optional<std::uint16_t> advertisedPort{};
};

// One bind address with optional HTTP and HTTPS service ports, independent of
// the wire protocol. App validates and expands this value atomically. An
// omitted port disables that service. autoHttpsRedirect targets the HTTPS
// service port. HTTP/3 does not have a separate public listener port.
struct ListenConfig final {
    // Numeric IPv4 or IPv6 bind address, normalized when App::listen consumes
    // this configuration.
    std::string address{"0.0.0.0"};
    std::optional<std::uint16_t> http{};
    std::optional<std::uint16_t> https{};
    TlsConfig tls{};
    Http3ListenConfig http3{};
    AltSvcConfig altSvc{};
    bool autoHttpsRedirect{false};
};

// Canonical startup values shared by App configuration and every worker's
// server options. They stay top-level so configuration is not copied between
// models.
struct CompressionConfig final {
    // Buffered in-memory responses below this size remain identity.
    std::size_t minBytes{1024};
    // Eligible bodies through this size are encoded synchronously on the
    // worker; larger bodies are offloaded to the bounded blocking pool when it
    // is enabled, otherwise they are also encoded synchronously.
    std::size_t syncBytes{std::size_t{64} * 1024};
    // Buffered bodies above this size remain identity. Static files use their
    // own document-root precompression thresholds and still prefer checked-in
    // precompressed sidecars when present.
    std::size_t maxBytes{std::size_t{64} * 1024 * 1024};
};

enum class CorsOriginMode : std::uint8_t {
    kAny,
    kExact,
    kCredentialedExact,
};

struct CorsOriginConfig final {
    CorsOriginMode mode{CorsOriginMode::kAny};
    // Required for exact modes. "null" represents the serialized opaque
    // origin; wildcard mode requires this field to remain empty.
    std::string value{};
};

enum class CorsRequestHeadersMode : std::uint8_t {
    kReflect,
    kFixed,
};

struct CorsRequestHeadersConfig final {
    CorsRequestHeadersMode mode{CorsRequestHeadersMode::kReflect};
    // Required in fixed mode and empty in reflect mode.
    std::vector<std::string> names{};
};

struct CorsConfig final {
    CorsOriginConfig origin{};
    CorsRequestHeadersConfig requestHeaders{};
    std::vector<std::string> exposeHeaders{};
    std::optional<std::chrono::seconds> maxAge{};
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
struct DeadlineConfig final {
    std::chrono::milliseconds handler{0};
};

// Runtime behavior belongs to the server's document-root binding, not to the
// immutable StaticRoot index. App document roots are always refreshed; a
// standalone StaticRoot remains immutable because it has no server runtime.
struct DocumentRootRuntimeConfig final {
    std::chrono::milliseconds refreshInterval{std::chrono::seconds(1)};
};

struct DocumentRootConfig final {
    std::filesystem::path root{};
    StaticRootOptions staticOptions{};
    DocumentRootRuntimeConfig runtime{};
    bool precompressGzip{false};
    bool precompressBrotli{false};
    bool precompressZstd{false};
    std::size_t precompressMinBytes{1024};
    std::size_t precompressMaxBytes{std::size_t{256} * 1024};
};

// One terminal response outcome with a committed final status, passed to the
// access-log callback after a complete buffered response head has reached the
// transport or a stream head is committed. The record borrows the immutable
// request and connection-owned remote address; the record and all returned views
// are valid only for the callback.
class AccessLogRecord final {
public:
    [[nodiscard]] std::string_view method() const noexcept {
        return request_.method();
    }

    [[nodiscard]] HttpKnownMethod knownMethod() const noexcept {
        return request_.knownMethod();
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return request_.path();
    }

    [[nodiscard]] constexpr std::string_view remoteAddress() const noexcept {
        return remoteAddress_;
    }

    [[nodiscard]] constexpr HttpStatusCode status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr std::uint64_t durationMicros() const noexcept {
        return durationMicros_;
    }

    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept {
        return request_.protocolVersion();
    }

private:
    friend struct detail::AccessLogRecordAccess;

    constexpr AccessLogRecord(const HttpRequest& request, std::string_view remoteAddress,
        HttpStatusCode status, std::uint64_t durationMicros) noexcept
        : request_(request),
          remoteAddress_(remoteAddress),
          status_(status),
          durationMicros_(durationMicros) {}

    const HttpRequest& request_;
    std::string_view remoteAddress_;
    HttpStatusCode status_;
    std::uint64_t durationMicros_;
};

namespace detail {
using AccessLogCallbackRef = CallbackRef<void(const AccessLogRecord&) noexcept>;
}  // namespace detail

// App-owned access-log listener. Request dispatch receives only an internal,
// allocation-free CallbackRef and never participates in this owner's lifetime.
using AccessLogCallback = detail::Callback<void(const AccessLogRecord&) noexcept>;

// One connection lost to an exception that escaped its session: a handler bug
// past the response's point of no return, an error handler that itself failed,
// or resource exhaustion. The server closes that connection and keeps serving;
// this record is how the failure becomes visible instead of vanishing with the
// connection. A request that fails before its response is committed never gets
// here -- it is answered with a 5xx through onError.
//
// Views and the record itself are valid only for the callback invocation.
class ConnectionFailureRecord final {
public:
    // Empty when the peer address could not be read (the connection was
    // already gone) or when the failure happened before it was resolved.
    [[nodiscard]] constexpr std::string_view remoteAddress() const noexcept {
        return remoteAddress_;
    }

    // Never null. Rethrow it to inspect the failure.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

private:
    friend struct detail::ConnectionFailureRecordAccess;

    ConnectionFailureRecord(std::string_view remoteAddress, std::exception_ptr exception) noexcept
        : remoteAddress_(remoteAddress),
          exception_(std::move(exception)) {}

    std::string_view remoteAddress_;
    std::exception_ptr exception_;
};

// HTTP serving counters, for health checks and metrics. Cumulative since the
// worker started and never reset, except activeConnections, which is a gauge.
// App::httpStats() sums these across every worker; each field is sampled
// independently, so treat them as a set of gauges rather than one snapshot.
//
// These make a server observable without installing any callback: onError sees
// request failures and onConnectionFailure sees lost connections, but neither
// answers "how many, since when".
struct HttpServerStats final {
    // Connections held right now, against HttpServerOptions::maxConnections.
    std::size_t activeConnections{0};
    // Connections closed on accept because that budget was full. A rising
    // count means the server is shedding load rather than queueing it.
    std::size_t connectionsRefused{0};
    // Connections lost to an exception, as delivered to onConnectionFailure.
    std::size_t connectionFailures{0};
    // Accepts that failed transiently (descriptor exhaustion, a session that
    // could not be started). Each cost one connection, not the listener.
    std::size_t acceptFailures{0};
    // Failures that escaped to the worker's io_context and stopped it.
    std::size_t workerFailures{0};
    // Polling refreshes whose replacement index could not be built. The
    // previous complete document-root snapshot remains active; this counter
    // makes filesystem/permission failures observable without taking the
    // worker down.
    std::size_t documentRootRefreshFailures{0};
};

namespace detail {
using ConnectionFailureCallbackRef = CallbackRef<void(const ConnectionFailureRecord&) noexcept>;
}  // namespace detail

// App-owned connection-failure listener. The listener must not throw: it runs on the last
// line of defense for a connection, where a second failure would have nowhere
// left to go, so the requirement is enforced at compile time rather than
// swallowed at runtime.
using ConnectionFailureCallback = detail::Callback<void(const ConnectionFailureRecord&) noexcept>;

}  // namespace ruvia
