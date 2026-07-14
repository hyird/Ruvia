#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <type_traits>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/web/StaticFiles.h"

namespace ruvia {

namespace detail {
struct AccessLogRecordAccess;
struct AccessLogSink;
}  // namespace detail

struct TlsConfig final {
    std::filesystem::path certificateChainFile;
    std::filesystem::path privateKeyFile;
    std::pmr::string privateKeyPassword;
    // A CA bundle used to verify a client certificate when one is presented. On
    // its own this enables OPTIONAL mutual TLS: a client with no certificate is
    // still accepted (its identity is surfaced empty via getConnInfo), while a
    // presented-but-untrusted certificate fails the handshake.
    std::filesystem::path verifyFile;
    // Require the client to present a certificate that verifyFile trusts, failing
    // the TLS handshake otherwise (mandatory mutual TLS). Has no effect without
    // verifyFile. Defaults false to preserve the optional-mTLS behavior above.
    bool requireClientCertificate{false};
};

// Canonical startup values shared by App setters and every worker's server
// options. They stay top-level so configuration is not copied between models.
struct CompressionConfig final {
    std::size_t minBytes{1024};
};

struct CorsConfig final {
    std::pmr::string allowOrigin{"*"};
    std::pmr::string allowHeaders;
    std::pmr::string exposeHeaders;
    std::optional<std::chrono::seconds> maxAge;
    bool allowCredentials{false};
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
