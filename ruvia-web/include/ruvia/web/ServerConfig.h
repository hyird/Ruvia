#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/Callback.h"
#include "ruvia/web/detail/CallbackRef.h"

namespace ruvia {

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
    std::optional<std::filesystem::path> verifyFile;
    TlsClientCertificateRequirement requirement{TlsClientCertificateRequirement::kOptional};
};

struct TlsSniConfig final {
    std::string host;
    std::filesystem::path certificateChainFile;
    std::filesystem::path privateKeyFile;
    std::string privateKeyPassword;
};

struct TlsConfig final {
    std::filesystem::path certificateChainFile;
    std::filesystem::path privateKeyFile;
    std::string privateKeyPassword;
    TlsClientCertificateConfig clientCertificates;
    std::vector<TlsSniConfig> sni;
};

// One bind address with optional HTTP and HTTPS endpoints. App validates and
// expands this value atomically, then gives every worker the same endpoints.
// An omitted port disables that transport. autoHttpsRedirect turns the HTTP
// endpoint into a redirect endpoint targeting the HTTPS port in this value.
struct ListenConfig final {
    std::string address{"0.0.0.0"};
    std::optional<std::uint16_t> http;
    std::optional<std::uint16_t> https;
    TlsConfig tls;
    bool autoHttpsRedirect{false};
};

// Canonical startup values shared by App setters and every worker's server
// options. They stay top-level so configuration is not copied between models.
struct CompressionConfig final {
    // Buffered in-memory responses below this size remain identity.
    std::size_t minBytes{1024};
    // Eligible bodies through this size are encoded synchronously on the
    // worker; larger bodies are offloaded to the bounded blocking pool when it
    // is enabled, otherwise they are also encoded synchronously.
    std::size_t syncBytes{64u * 1024u};
    // Buffered bodies above this size remain identity. Static files ignore all
    // three thresholds and only negotiate existing precompressed sidecars.
    std::size_t maxBytes{64u * 1024u * 1024u};
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
    std::string value;
};

enum class CorsRequestHeadersMode : std::uint8_t {
    kReflect,
    kFixed,
};

struct CorsRequestHeadersConfig final {
    CorsRequestHeadersMode mode{CorsRequestHeadersMode::kReflect};
    // Required in fixed mode and empty in reflect mode.
    std::vector<std::string> names;
};

struct CorsConfig final {
    CorsOriginConfig origin;
    CorsRequestHeadersConfig requestHeaders;
    std::vector<std::string> exposeHeaders;
    std::optional<std::chrono::seconds> maxAge;
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
    std::optional<std::chrono::milliseconds> handler;
};

// Runtime behavior belongs to the server's document-root binding, not to the
// immutable StaticRoot index. App document roots are always refreshed; a
// standalone StaticRoot remains immutable because it has no server runtime.
struct DocumentRootRuntimeOptions final {
    std::chrono::milliseconds refreshInterval{std::chrono::seconds(1)};
};

struct DocumentRootConfig final {
    std::filesystem::path root;
    StaticRootOptions staticOptions;
    DocumentRootRuntimeOptions runtimeOptions;
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

    constexpr AccessLogRecord(const HttpRequest& request, std::string_view remoteAddress, HttpStatusCode status, std::uint64_t durationMicros) noexcept
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
