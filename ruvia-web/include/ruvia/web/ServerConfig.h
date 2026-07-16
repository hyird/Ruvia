#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
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
#include "ruvia/web/StaticFiles.h"

namespace ruvia {

namespace detail {
struct AccessLogRecordAccess;
struct AccessLogSink;
}  // namespace detail

class TlsIdentity final {
public:
    [[nodiscard]] static TlsIdentity fromFiles(
        std::filesystem::path certificateChainFile,
        std::filesystem::path privateKeyFile,
        std::pmr::string privateKeyPassword = {});

    [[nodiscard]] const std::filesystem::path& certificateChainFile() const & noexcept {
        return certificateChainFile_;
    }
    const std::filesystem::path& certificateChainFile() const && = delete;

    [[nodiscard]] const std::filesystem::path& privateKeyFile() const & noexcept {
        return privateKeyFile_;
    }
    const std::filesystem::path& privateKeyFile() const && = delete;

    [[nodiscard]] const std::pmr::string& privateKeyPassword() const & noexcept {
        return privateKeyPassword_;
    }
    const std::pmr::string& privateKeyPassword() const && = delete;

private:
    TlsIdentity(
        std::filesystem::path certificateChainFile,
        std::filesystem::path privateKeyFile,
        std::pmr::string privateKeyPassword) noexcept
        : certificateChainFile_(std::move(certificateChainFile)),
          privateKeyFile_(std::move(privateKeyFile)),
          privateKeyPassword_(std::move(privateKeyPassword)) {}

    std::filesystem::path certificateChainFile_;
    std::filesystem::path privateKeyFile_;
    std::pmr::string privateKeyPassword_;
};

enum class TlsClientCertificateRequirement : std::uint8_t {
    kOptional,
    kRequired,
};

class TlsClientCertificatePolicy final {
public:
    // A CA bundle used to verify presented client certificates. Optional mode
    // admits a client without a certificate; required mode rejects it.
    [[nodiscard]] static TlsClientCertificatePolicy optional(
        std::filesystem::path verifyFile);
    [[nodiscard]] static TlsClientCertificatePolicy required(
        std::filesystem::path verifyFile);

    [[nodiscard]] const std::filesystem::path& verifyFile() const & noexcept {
        return verifyFile_;
    }
    const std::filesystem::path& verifyFile() const && = delete;

    [[nodiscard]] constexpr TlsClientCertificateRequirement requirement()
        const noexcept {
        return requirement_;
    }

private:
    TlsClientCertificatePolicy(
        std::filesystem::path verifyFile,
        TlsClientCertificateRequirement requirement) noexcept
        : verifyFile_(std::move(verifyFile)), requirement_(requirement) {}

    std::filesystem::path verifyFile_;
    TlsClientCertificateRequirement requirement_;
};

class TlsSniIdentity final {
public:
    [[nodiscard]] std::string_view host() const & noexcept { return host_; }
    std::string_view host() const && = delete;
    [[nodiscard]] const TlsIdentity& identity() const & noexcept { return identity_; }
    const TlsIdentity& identity() const && = delete;

private:
    friend class TlsConfig;

    TlsSniIdentity(std::pmr::string host, TlsIdentity identity) noexcept
        : host_(std::move(host)), identity_(std::move(identity)) {}

    std::pmr::string host_;
    TlsIdentity identity_;
};

class TlsConfig final {
public:
    explicit TlsConfig(TlsIdentity identity) noexcept
        : identity_(std::move(identity)) {}

    TlsConfig& setClientCertificatePolicy(TlsClientCertificatePolicy policy);
    TlsConfig& addSniIdentity(std::string_view host, TlsIdentity identity);

    [[nodiscard]] const TlsIdentity& identity() const & noexcept { return identity_; }
    const TlsIdentity& identity() const && = delete;

    [[nodiscard]] const std::optional<TlsClientCertificatePolicy>&
    clientCertificatePolicy() const & noexcept {
        return clientCertificatePolicy_;
    }
    const std::optional<TlsClientCertificatePolicy>&
    clientCertificatePolicy() const && = delete;

    [[nodiscard]] const std::pmr::vector<TlsSniIdentity>& sniIdentities()
        const & noexcept {
        return sniIdentities_;
    }
    const std::pmr::vector<TlsSniIdentity>& sniIdentities() const && = delete;

private:
    TlsIdentity identity_;
    std::optional<TlsClientCertificatePolicy> clientCertificatePolicy_;
    std::pmr::vector<TlsSniIdentity> sniIdentities_;
};

// The complete listener graph is selected atomically. HTTPS and redirect
// topologies always carry their TLS identity, so App cannot observe a partially
// configured listener/TLS combination.
class ServerTopology final {
public:
    ServerTopology() noexcept = default;

    [[nodiscard]] static ServerTopology http(std::uint16_t port = 8080);
    [[nodiscard]] static ServerTopology https(
        std::uint16_t port,
        TlsConfig tls);
    [[nodiscard]] static ServerTopology httpAndHttps(
        std::uint16_t httpPort,
        std::uint16_t httpsPort,
        TlsConfig tls);
    [[nodiscard]] static ServerTopology redirectHttpToHttps(
        std::uint16_t httpPort,
        std::uint16_t httpsPort,
        TlsConfig tls);

private:
    friend class App;

    struct Http final {
        std::uint16_t port;
    };

    struct Https final {
        std::uint16_t port;
        TlsConfig tls;
    };

    struct HttpAndHttps final {
        std::uint16_t httpPort;
        std::uint16_t httpsPort;
        TlsConfig tls;
    };

    struct RedirectHttpToHttps final {
        std::uint16_t httpPort;
        std::uint16_t httpsPort;
        TlsConfig tls;
    };

    using Topology = std::variant<
        Http,
        Https,
        HttpAndHttps,
        RedirectHttpToHttps>;

    explicit ServerTopology(Topology topology) noexcept
        : topology_(std::move(topology)) {}

    Topology topology_{Http{8080}};
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

    [[nodiscard]] std::string_view value() const & noexcept {
        return value_;
    }
    std::string_view value() const && = delete;

private:
    friend class CorsOriginPolicy;

    explicit CorsOrigin(std::pmr::string value) noexcept
        : value_(std::move(value)) {}

    std::pmr::string value_;
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
        return CorsOriginPolicy(
            Kind::kCredentialedExact,
            std::move(origin.value_));
    }

    [[nodiscard]] constexpr Kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::string_view origin() const & noexcept {
        return value_;
    }
    std::string_view origin() const && = delete;

private:
    CorsOriginPolicy(Kind kind, std::pmr::string value) noexcept
        : kind_(kind), value_(std::move(value)) {}

    Kind kind_;
    std::pmr::string value_;
};

class CorsHeaderNames final {
public:
    CorsHeaderNames() noexcept = default;

    [[nodiscard]] static CorsHeaderNames of(
        std::span<const std::string_view> names);
    [[nodiscard]] static CorsHeaderNames of(
        std::initializer_list<std::string_view> names) {
        return of(std::span<const std::string_view>(names.begin(), names.size()));
    }

    [[nodiscard]] std::string_view value() const & noexcept {
        return value_;
    }
    std::string_view value() const && = delete;

    [[nodiscard]] bool empty() const noexcept {
        return value_.empty();
    }

private:
    explicit CorsHeaderNames(std::pmr::string value) noexcept
        : value_(std::move(value)) {}

    std::pmr::string value_;
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

    [[nodiscard]] static CorsRequestHeadersPolicy fixed(
        CorsHeaderNames headers) {
        if (headers.empty()) {
            throw std::invalid_argument(
                "CORS fixed request headers must not be empty");
        }
        return CorsRequestHeadersPolicy(Kind::kFixed, std::move(headers));
    }

    [[nodiscard]] static CorsRequestHeadersPolicy fixed(
        std::span<const std::string_view> headers) {
        return fixed(CorsHeaderNames::of(headers));
    }

    [[nodiscard]] static CorsRequestHeadersPolicy fixed(
        std::initializer_list<std::string_view> headers) {
        return fixed(std::span<const std::string_view>(
            headers.begin(),
            headers.size()));
    }

    [[nodiscard]] constexpr Kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] std::string_view headers() const & noexcept {
        return headers_.value();
    }
    std::string_view headers() const && = delete;

private:
    CorsRequestHeadersPolicy(Kind kind, CorsHeaderNames headers) noexcept
        : kind_(kind), headers_(std::move(headers)) {}

    Kind kind_;
    CorsHeaderNames headers_;
};

class CorsMaxAge final {
public:
    explicit CorsMaxAge(std::chrono::seconds value) : value_(value) {
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

struct DocumentRootConfig final {
    std::filesystem::path root;
    StaticRootOptions staticOptions;
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

    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
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

    constexpr AccessLogRecord(
        const HttpRequest& request,
        std::string_view remoteAddress,
        std::uint16_t status,
        std::uint64_t durationMicros) noexcept
        : request_(request),
          remoteAddress_(remoteAddress),
          status_(status),
          durationMicros_(durationMicros) {}

    const HttpRequest& request_;
    std::string_view remoteAddress_;
    std::uint16_t status_;
    std::uint64_t durationMicros_;
};

// A non-owning, allocation-free access-log listener. The bound object must
// outlive App::run(); the request hot path performs one null check and one
// function-pointer call.
class AccessLogCallback final {
public:
    constexpr AccessLogCallback() noexcept = default;

    template <typename Listener>
    requires (!std::is_function_v<Listener> &&
              std::is_nothrow_invocable_r_v<void, Listener&, const AccessLogRecord&>)
    [[nodiscard]] static constexpr AccessLogCallback bind(Listener& listener) noexcept {
        return AccessLogCallback(
            std::addressof(listener),
            [](void* target, const AccessLogRecord& record) noexcept {
                (*static_cast<Listener*>(target))(record);
            });
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

private:
    friend struct detail::AccessLogRecordAccess;
    friend struct detail::AccessLogSink;

    using Invoke = void (*)(void*, const AccessLogRecord&) noexcept;

    constexpr AccessLogCallback(void* target, Invoke invoke) noexcept
        : target_(target),
          invoke_(invoke) {}

    void invoke(const AccessLogRecord& record) const noexcept {
        invoke_(target_, record);
    }

    void* target_{nullptr};
    Invoke invoke_{nullptr};
};

}  // namespace ruvia
