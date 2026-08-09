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

class TlsIdentity final {
public:
    [[nodiscard]] static TlsIdentity fromFiles(std::filesystem::path certificateChainFile, std::filesystem::path privateKeyFile, std::string_view privateKeyPassword = {});

    [[nodiscard]] const std::filesystem::path& certificateChainFile() const& noexcept {
        return certificateChainFile_;
    }
    const std::filesystem::path& certificateChainFile() const&& = delete;

    [[nodiscard]] const std::filesystem::path& privateKeyFile() const& noexcept {
        return privateKeyFile_;
    }
    const std::filesystem::path& privateKeyFile() const&& = delete;

    [[nodiscard]] const std::string& privateKeyPassword() const& noexcept {
        return privateKeyPassword_;
    }
    const std::string& privateKeyPassword() const&& = delete;

private:
    TlsIdentity(std::filesystem::path certificateChainFile, std::filesystem::path privateKeyFile, std::string privateKeyPassword) noexcept
        : certificateChainFile_(std::move(certificateChainFile)),
          privateKeyFile_(std::move(privateKeyFile)),
          privateKeyPassword_(std::move(privateKeyPassword)) {}

    std::filesystem::path certificateChainFile_;
    std::filesystem::path privateKeyFile_;
    std::string privateKeyPassword_;
};

enum class TlsClientCertificateRequirement : std::uint8_t {
    kOptional,
    kRequired,
};

class TlsClientCertificatePolicy final {
public:
    // A CA bundle used to verify presented client certificates. Optional mode
    // admits a client without a certificate; required mode rejects it.
    [[nodiscard]] static TlsClientCertificatePolicy optional(std::filesystem::path verifyFile);
    [[nodiscard]] static TlsClientCertificatePolicy required(std::filesystem::path verifyFile);

    [[nodiscard]] const std::filesystem::path& verifyFile() const& noexcept {
        return verifyFile_;
    }
    const std::filesystem::path& verifyFile() const&& = delete;

    [[nodiscard]] constexpr TlsClientCertificateRequirement requirement() const noexcept {
        return requirement_;
    }

private:
    TlsClientCertificatePolicy(std::filesystem::path verifyFile, TlsClientCertificateRequirement requirement) noexcept
        : verifyFile_(std::move(verifyFile)),
          requirement_(requirement) {}

    std::filesystem::path verifyFile_;
    TlsClientCertificateRequirement requirement_;
};

class TlsSniIdentity final {
public:
    [[nodiscard]] std::string_view host() const& noexcept {
        return host_;
    }
    std::string_view host() const&& = delete;
    [[nodiscard]] const TlsIdentity& identity() const& noexcept {
        return identity_;
    }
    const TlsIdentity& identity() const&& = delete;

private:
    friend class TlsConfig;

    TlsSniIdentity(std::string host, TlsIdentity identity) noexcept
        : host_(std::move(host)),
          identity_(std::move(identity)) {}

    std::string host_;
    TlsIdentity identity_;
};

class TlsConfig final {
public:
    explicit TlsConfig(TlsIdentity identity) noexcept
        : identity_(std::move(identity)) {}

    TlsConfig& setClientCertificatePolicy(TlsClientCertificatePolicy policy);
    TlsConfig& addSniIdentity(std::string_view host, TlsIdentity identity);

    [[nodiscard]] const TlsIdentity& identity() const& noexcept {
        return identity_;
    }
    const TlsIdentity& identity() const&& = delete;

    [[nodiscard]] const std::optional<TlsClientCertificatePolicy>& clientCertificatePolicy() const& noexcept {
        return clientCertificatePolicy_;
    }
    const std::optional<TlsClientCertificatePolicy>& clientCertificatePolicy() const&& = delete;

    [[nodiscard]] const std::vector<TlsSniIdentity>& sniIdentities() const& noexcept {
        return sniIdentities_;
    }
    const std::vector<TlsSniIdentity>& sniIdentities() const&& = delete;

private:
    TlsIdentity identity_;
    std::optional<TlsClientCertificatePolicy> clientCertificatePolicy_;
    std::vector<TlsSniIdentity> sniIdentities_;
};

// One independently replicated listener. App validates the complete listener
// list atomically, including unique ports and redirect targets, before storing
// it. This scales beyond the old fixed one/two-listener combinations.
class ListenerConfig final {
public:
    [[nodiscard]] static ListenerConfig http(std::string_view address = "0.0.0.0", std::uint16_t port = 8080);
    [[nodiscard]] static ListenerConfig https(std::string_view address, std::uint16_t port, TlsConfig tls);
    [[nodiscard]] static ListenerConfig redirectHttpToHttps(std::string_view address, std::uint16_t port, std::uint16_t targetHttpsPort);

private:
    friend class App;

    struct Http final {
        std::string address;
        std::uint16_t port;
    };

    struct Https final {
        std::string address;
        std::uint16_t port;
        TlsConfig tls;
    };

    struct RedirectHttpToHttps final {
        std::string address;
        std::uint16_t port;
        std::uint16_t targetHttpsPort;
    };

    using Listener = std::variant<Http, Https, RedirectHttpToHttps>;

    explicit ListenerConfig(Listener listener) noexcept
        : listener_(std::move(listener)) {}

    Listener listener_;
};

// Canonical startup values shared by App setters and every worker's server
// options. They stay top-level so configuration is not copied between models.
struct CompressionConfig final {
    std::size_t minBytes{1024};
};

class CorsOrigin final {
public:
    [[nodiscard]] static CorsOrigin serialized(std::string_view value);
    [[nodiscard]] static CorsOrigin opaque();

    [[nodiscard]] std::string_view value() const& noexcept {
        return value_;
    }
    std::string_view value() const&& = delete;

private:
    friend class CorsOriginPolicy;

    explicit CorsOrigin(std::string value) noexcept
        : value_(std::move(value)) {}

    std::string value_;
};

class CorsOriginPolicy final {
public:
    enum class Kind : std::uint8_t {
        kAny,
        kExact,
        kCredentialedExact,
    };

    [[nodiscard]] static CorsOriginPolicy any() {
        return CorsOriginPolicy(Kind::kAny, {});
    }

    [[nodiscard]] static CorsOriginPolicy exact(CorsOrigin origin) {
        return CorsOriginPolicy(Kind::kExact, std::move(origin.value_));
    }

    [[nodiscard]] static CorsOriginPolicy credentialed(CorsOrigin origin) {
        return CorsOriginPolicy(Kind::kCredentialedExact, std::move(origin.value_));
    }

    [[nodiscard]] constexpr Kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::string_view origin() const& noexcept {
        return value_;
    }
    std::string_view origin() const&& = delete;

private:
    CorsOriginPolicy(Kind kind, std::string value) noexcept
        : kind_(kind),
          value_(std::move(value)) {}

    Kind kind_;
    std::string value_;
};

class CorsHeaderNames final {
public:
    CorsHeaderNames() noexcept = default;

    [[nodiscard]] static CorsHeaderNames of(std::span<const std::string_view> names);
    [[nodiscard]] static CorsHeaderNames of(std::initializer_list<std::string_view> names) {
        return of(std::span<const std::string_view>(names.begin(), names.size()));
    }

    [[nodiscard]] std::string_view value() const& noexcept {
        return value_;
    }
    std::string_view value() const&& = delete;

    [[nodiscard]] bool empty() const noexcept {
        return value_.empty();
    }

private:
    explicit CorsHeaderNames(std::string value) noexcept
        : value_(std::move(value)) {}

    std::string value_;
};

class CorsRequestHeadersPolicy final {
public:
    enum class Kind : std::uint8_t {
        kReflect,
        kFixed,
    };

    [[nodiscard]] static CorsRequestHeadersPolicy reflect() {
        return CorsRequestHeadersPolicy(Kind::kReflect, CorsHeaderNames{});
    }

    [[nodiscard]] static CorsRequestHeadersPolicy fixed(CorsHeaderNames headers) {
        if (headers.empty()) {
            throw std::invalid_argument("CORS fixed request headers must not be empty");
        }
        return CorsRequestHeadersPolicy(Kind::kFixed, std::move(headers));
    }

    [[nodiscard]] static CorsRequestHeadersPolicy fixed(std::span<const std::string_view> headers) {
        return fixed(CorsHeaderNames::of(headers));
    }

    [[nodiscard]] static CorsRequestHeadersPolicy fixed(std::initializer_list<std::string_view> headers) {
        return fixed(std::span<const std::string_view>(headers.begin(), headers.size()));
    }

    [[nodiscard]] constexpr Kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] std::string_view headers() const& noexcept {
        return headers_.value();
    }
    std::string_view headers() const&& = delete;

private:
    CorsRequestHeadersPolicy(Kind kind, CorsHeaderNames headers) noexcept
        : kind_(kind),
          headers_(std::move(headers)) {}

    Kind kind_;
    CorsHeaderNames headers_;
};

class CorsMaxAge final {
public:
    explicit CorsMaxAge(std::chrono::seconds value)
        : value_(value) {
        if (value.count() < 0) {
            throw std::invalid_argument("CORS max age must not be negative");
        }
    }

    [[nodiscard]] constexpr std::chrono::seconds value() const noexcept {
        return value_;
    }

private:
    std::chrono::seconds value_;
};

struct CorsConfig final {
    CorsOriginPolicy origin{CorsOriginPolicy::any()};
    CorsRequestHeadersPolicy requestHeaders{CorsRequestHeadersPolicy::reflect()};
    CorsHeaderNames exposeHeaders;
    std::optional<CorsMaxAge> maxAge;
};

enum class DocumentRootRefreshMode : std::uint8_t {
    kImmutable,
    kPolling,
};

// Runtime behavior belongs to the server's document-root binding, not to the
// immutable StaticRoot index. A standalone StaticRoot therefore cannot
// accidentally advertise a refresh or compression policy that nobody runs.
struct DocumentRootRuntimeOptions final {
    DocumentRootRefreshMode refreshMode{DocumentRootRefreshMode::kImmutable};
    std::chrono::milliseconds refreshInterval{std::chrono::seconds(1)};
    // When an accepted coding has no sidecar, the Web runtime may compress a
    // small complete file through the blocking pool. Zero disables this
    // fallback; larger files remain identity unless a sidecar exists.
    std::size_t onDemandCompressionMaxBytes{2u * 1024u * 1024u};
    // Development-only browser refresh support. The Web runtime exposes a
    // small version endpoint and a polling script; applications opt in by
    // including the script in their HTML.
    bool enableLiveReload{false};
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
