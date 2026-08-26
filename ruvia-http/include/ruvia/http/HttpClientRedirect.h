#pragma once

// Outbound HTTP redirect protocol helpers.
//
// These values contain no transport policy: redirect limits, cross-origin
// authorization, connection selection, and retries remain owned by the external
// I/O runtime. The helpers only classify HTTP response fields, apply the RFC
// method/content rewrite rules, and resolve one same-origin URI-reference.

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpClient.h"

namespace ruvia {

[[nodiscard]] bool isValidHttpClientOriginTarget(std::string_view target) noexcept;

[[nodiscard]] bool isHttpClientRedirectStatus(HttpStatusCode status) noexcept;

class HttpClientResponseHeaderLookupResult;

class HttpClientResponseHeaderAbsent final {
private:
    friend class HttpClientResponseHeaderLookupResult;
    constexpr HttpClientResponseHeaderAbsent() noexcept = default;
};

class HttpClientResponseHeaderFound final {
public:
    // Borrowed from the owning HttpClientResponseHead passed to the lookup.
    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    friend class HttpClientResponseHeaderLookupResult;

    explicit constexpr HttpClientResponseHeaderFound(std::string_view value) noexcept
        : value_(value) {}

    std::string_view value_;
};

class HttpClientResponseHeaderRepeated final {
private:
    friend class HttpClientResponseHeaderLookupResult;
    constexpr HttpClientResponseHeaderRepeated() noexcept = default;
};

// A non-list response field has three mutually exclusive lookup outcomes. Only
// the found alternative exposes a value, so an absent/repeated field cannot be
// mistaken for a present field whose value is empty.
class HttpClientResponseHeaderLookupResult final {
public:
    [[nodiscard]] constexpr const HttpClientResponseHeaderAbsent* absent() const& noexcept {
        return std::get_if<HttpClientResponseHeaderAbsent>(&value_);
    }
    const HttpClientResponseHeaderAbsent* absent() const&& = delete;

    [[nodiscard]] constexpr const HttpClientResponseHeaderFound* found() const& noexcept {
        return std::get_if<HttpClientResponseHeaderFound>(&value_);
    }
    const HttpClientResponseHeaderFound* found() const&& = delete;

    [[nodiscard]] constexpr const HttpClientResponseHeaderRepeated* repeated() const& noexcept {
        return std::get_if<HttpClientResponseHeaderRepeated>(&value_);
    }
    const HttpClientResponseHeaderRepeated* repeated() const&& = delete;

private:
    friend HttpClientResponseHeaderLookupResult lookupUniqueHttpClientResponseHeader(
        const HttpClientResponseHead&, std::string_view) noexcept;

    using Value = std::variant<HttpClientResponseHeaderAbsent, HttpClientResponseHeaderFound,
        HttpClientResponseHeaderRepeated>;

    template <typename Result>
    explicit constexpr HttpClientResponseHeaderLookupResult(Result result) noexcept
        : value_(result) {}

    [[nodiscard]] static constexpr HttpClientResponseHeaderLookupResult makeAbsent() noexcept {
        return HttpClientResponseHeaderLookupResult(HttpClientResponseHeaderAbsent());
    }

    [[nodiscard]] static constexpr HttpClientResponseHeaderLookupResult makeFound(
        std::string_view value) noexcept {
        return HttpClientResponseHeaderLookupResult(HttpClientResponseHeaderFound(value));
    }

    [[nodiscard]] static constexpr HttpClientResponseHeaderLookupResult makeRepeated() noexcept {
        return HttpClientResponseHeaderLookupResult(HttpClientResponseHeaderRepeated());
    }

    Value value_;
};

[[nodiscard]] HttpClientResponseHeaderLookupResult lookupUniqueHttpClientResponseHeader(
    const HttpClientResponseHead& head, std::string_view name) noexcept;
[[nodiscard]] HttpClientResponseHeaderLookupResult lookupUniqueHttpClientResponseHeader(
    const HttpClientResponseHead&& head, std::string_view name) = delete;

enum class HttpClientRedirectContentDisposition : std::uint8_t {
    kPreserve,
    // The I/O owner must omit both the representation and content-specific
    // fields when constructing the redirected request (RFC 9110 Section 15.4).
    kDrop,
};

struct HttpClientRedirectRequestPlanOptions final {
    HttpStatusCode status;
    std::pmr::memory_resource* resource{nullptr};
};

// The method is copied into caller-selected PMR storage so the plan does not
// inherit the request or request-method backing storage lifetime.
class HttpClientRedirectRequestPlan final {
public:
    HttpClientRedirectRequestPlan(const HttpClientRedirectRequestPlan&) = delete;
    HttpClientRedirectRequestPlan& operator=(const HttpClientRedirectRequestPlan&) = delete;
    HttpClientRedirectRequestPlan(HttpClientRedirectRequestPlan&&) noexcept = default;
    HttpClientRedirectRequestPlan& operator=(HttpClientRedirectRequestPlan&&) = delete;

    [[nodiscard]] std::string_view method() const& noexcept {
        return method_;
    }
    std::string_view method() const&& = delete;

    [[nodiscard]] constexpr HttpClientRedirectContentDisposition contentDisposition()
        const noexcept {
        return contentDisposition_;
    }

private:
    friend HttpClientRedirectRequestPlan planHttpClientRedirectRequest(
        const HttpClientRequestView&, HttpClientRedirectRequestPlanOptions);

    HttpClientRedirectRequestPlan(std::string_view method,
        HttpClientRedirectContentDisposition contentDisposition,
        std::pmr::memory_resource* resource);

    std::pmr::string method_;
    HttpClientRedirectContentDisposition contentDisposition_;
};

[[nodiscard]] HttpClientRedirectRequestPlan planHttpClientRedirectRequest(
    const HttpClientRequestView& request, HttpClientRedirectRequestPlanOptions options);

// This classification has no alternative-specific payload, so an enum is the
// complete result rather than a status coupled to unrelated fields.
enum class HttpClientOriginAuthorityStatus : std::uint8_t {
    kSameOrigin,
    kDifferentOrigin,
    kInvalidAuthority,
};

[[nodiscard]] HttpClientOriginAuthorityStatus classifyHttpClientOriginAuthority(
    const HttpOriginView& origin, std::string_view authority) noexcept;

enum class HttpClientRedirectResolutionError : std::uint8_t {
    kInvalidCurrentTarget,
    kInvalidLocation,
    // The Location names a scheme this client cannot follow (anything other
    // than http/https). The response itself is well-formed; the I/O owner
    // decides whether to surface the response or fail the exchange.
    kUnsupportedScheme,
};

struct HttpClientRedirectTargetOptions final {
    std::string_view currentTarget{};
    std::string_view location{};
    std::pmr::memory_resource* resource{nullptr};
};

class HttpClientRedirectResolutionResult;

// A followable redirect destination: the resolved origin plus the origin-form
// target on that origin. `crossOrigin()` is true whenever the destination
// scheme/host/port triple differs from the request origin; RFC 9110 Section
// 15.4 and the fetch specification require the I/O owner to drop credentials
// (Authorization, Proxy-Authorization, cookie material not scoped to the new
// origin) before following a cross-origin redirect.
class HttpClientResolvedRedirect final {
public:
    HttpClientResolvedRedirect(const HttpClientResolvedRedirect&) = delete;
    HttpClientResolvedRedirect& operator=(const HttpClientResolvedRedirect&) = delete;
    HttpClientResolvedRedirect(HttpClientResolvedRedirect&&) noexcept = default;
    HttpClientResolvedRedirect& operator=(HttpClientResolvedRedirect&&) = delete;

    [[nodiscard]] std::string_view target() const& noexcept {
        return target_;
    }
    std::string_view target() const&& = delete;

    [[nodiscard]] HttpScheme scheme() const noexcept {
        return scheme_;
    }

    // RFC 3986 uri-host of the destination; IP literals keep their brackets,
    // matching the HttpOriginView factory contract.
    [[nodiscard]] std::string_view host() const& noexcept {
        return host_;
    }
    std::string_view host() const&& = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] bool crossOrigin() const noexcept {
        return crossOrigin_;
    }

    // Borrows host() storage: the returned origin is valid only while this
    // resolved redirect is alive.
    [[nodiscard]] HttpOriginView origin() const&;
    HttpOriginView origin() const&& = delete;

private:
    friend class HttpClientRedirectResolutionResult;

    HttpClientResolvedRedirect(HttpScheme scheme, std::pmr::string host, std::uint16_t port,
        std::pmr::string target, bool crossOrigin) noexcept
        : scheme_(scheme),
          host_(std::move(host)),
          port_(port),
          target_(std::move(target)),
          crossOrigin_(crossOrigin) {}

    HttpScheme scheme_;
    std::pmr::string host_;
    std::uint16_t port_;
    std::pmr::string target_;
    bool crossOrigin_;
};

class HttpClientRedirectResolutionFailure final {
public:
    [[nodiscard]] constexpr HttpClientRedirectResolutionError error() const noexcept {
        return error_;
    }

private:
    friend class HttpClientRedirectResolutionResult;

    explicit constexpr HttpClientRedirectResolutionFailure(
        HttpClientRedirectResolutionError error) noexcept
        : error_(error) {}

    HttpClientRedirectResolutionError error_;
};

// Cross-origin-capable resolution: either one owned followable destination or
// one typed failure. A different http/https origin is a success alternative
// classified by crossOrigin(); the I/O owner applies its own cross-origin
// policy (credential strip, TLS-downgrade refusal) on top of the classified
// destination.
class HttpClientRedirectResolutionResult final {
public:
    HttpClientRedirectResolutionResult(const HttpClientRedirectResolutionResult&) = delete;
    HttpClientRedirectResolutionResult& operator=(
        const HttpClientRedirectResolutionResult&) = delete;
    HttpClientRedirectResolutionResult(HttpClientRedirectResolutionResult&&) noexcept = default;
    HttpClientRedirectResolutionResult& operator=(HttpClientRedirectResolutionResult&&) = delete;

    [[nodiscard]] const HttpClientResolvedRedirect* resolved() const& noexcept {
        return std::get_if<HttpClientResolvedRedirect>(&value_);
    }
    const HttpClientResolvedRedirect* resolved() const&& = delete;

    [[nodiscard]] constexpr const HttpClientRedirectResolutionFailure* failure() const& noexcept {
        return std::get_if<HttpClientRedirectResolutionFailure>(&value_);
    }
    const HttpClientRedirectResolutionFailure* failure() const&& = delete;

private:
    friend HttpClientRedirectResolutionResult resolveHttpClientRedirectTarget(
        const HttpOriginView&, HttpClientRedirectTargetOptions);

    using Value = std::variant<HttpClientResolvedRedirect, HttpClientRedirectResolutionFailure>;

    explicit HttpClientRedirectResolutionResult(HttpClientResolvedRedirect resolved) noexcept
        : value_(std::move(resolved)) {}

    explicit constexpr HttpClientRedirectResolutionResult(
        HttpClientRedirectResolutionFailure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static HttpClientRedirectResolutionResult makeResolved(HttpScheme scheme,
        std::pmr::string host, std::uint16_t port, std::pmr::string target,
        bool crossOrigin) noexcept {
        return HttpClientRedirectResolutionResult(HttpClientResolvedRedirect(
            scheme, std::move(host), port, std::move(target), crossOrigin));
    }

    [[nodiscard]] static constexpr HttpClientRedirectResolutionResult makeFailure(
        HttpClientRedirectResolutionError error) noexcept {
        return HttpClientRedirectResolutionResult(HttpClientRedirectResolutionFailure(error));
    }

    Value value_;
};

[[nodiscard]] HttpClientRedirectResolutionResult resolveHttpClientRedirectTarget(
    const HttpOriginView& origin, HttpClientRedirectTargetOptions options);

}  // namespace ruvia
