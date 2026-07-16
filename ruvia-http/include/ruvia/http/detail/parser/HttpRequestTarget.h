#pragma once

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/BorrowedView.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ruvia::detail {

struct RequestTargetView {
    std::string_view path;
    std::string_view query;
    std::string_view authority;
    std::uint16_t defaultPort{0};
};

enum class HttpAuthorityPortKind : std::uint8_t {
    kAbsent,
    kEmpty,
    kValue,
};

struct HttpAuthorityViewAccess;

// A validated RFC 3986 authority without userinfo. Keeping an explicit empty
// port distinct from an absent port is required for syntax-preserving parsing;
// both map to the scheme default when an HTTP origin is compared.
class HttpAuthorityView final {
public:
    [[nodiscard]] constexpr std::string_view host() const noexcept {
        return host_;
    }

    [[nodiscard]] constexpr HttpAuthorityPortKind portKind() const noexcept {
        return portKind_;
    }

    [[nodiscard]] constexpr std::optional<std::uint16_t> port() const noexcept {
        return portKind_ == HttpAuthorityPortKind::kValue
            ? std::optional<std::uint16_t>(port_)
            : std::nullopt;
    }

    [[nodiscard]] constexpr std::uint16_t effectivePort(
        std::uint16_t defaultPort) const noexcept {
        return portKind_ == HttpAuthorityPortKind::kValue ? port_ : defaultPort;
    }

private:
    friend struct HttpAuthorityViewAccess;

    constexpr HttpAuthorityView(
        std::string_view host,
        HttpAuthorityPortKind portKind,
        std::uint16_t port) noexcept
        : host_(host), port_(port), portKind_(portKind) {}

    std::string_view host_;
    std::uint16_t port_{0};
    HttpAuthorityPortKind portKind_{HttpAuthorityPortKind::kAbsent};
};

// Validates the byte repertoire shared by the four HTTP request-target forms.
// Component-specific rules (for example, '[' and ']' being legal only in an
// IP-literal authority) are applied by parseRequestTarget /
// isValidOriginFormTarget.
[[nodiscard]] bool isValidRequestTargetBytes(std::string_view target) noexcept;

[[nodiscard]] bool isValidOriginFormTarget(std::string_view target) noexcept;

[[nodiscard]] bool isValidHostHeader(std::string_view value) noexcept;
// RFC 3986 `host` / HTTP `uri-host`, without a port. IP literals use their
// standard bracketed form; IPv6 zone identifiers are not URI syntax (RFC 9844
// reverted RFC 6874), while IPvFuture remains valid.
[[nodiscard]] bool isValidHttpHost(std::string_view value) noexcept;
// WHATWG Fetch `serialized-origin` syntax. Unlike an RFC 3986 uri-host, this
// wire value requires lowercase scheme/domain bytes and canonical IPv6 groups,
// and never contains a path. The opaque-origin literal `null` is deliberately
// not a serialized origin.
[[nodiscard]] bool isValidHttpSerializedOrigin(
    std::string_view value) noexcept;
[[nodiscard]] std::optional<HttpAuthorityView> parseHttpAuthority(
    std::string_view value) noexcept;
template <HttpTemporaryOwningCharString Value>
std::optional<HttpAuthorityView> parseHttpAuthority(Value&&) = delete;
[[nodiscard]] bool httpUriHostEquals(
    std::string_view left,
    std::string_view right) noexcept;
[[nodiscard]] bool parseRequestTarget(
    HttpKnownMethod method,
    std::string_view target,
    RequestTargetView& output) noexcept;
[[nodiscard]] bool authorityMatchesHost(
    std::string_view authority,
    std::string_view host,
    std::uint16_t defaultPort) noexcept;

}  // namespace ruvia::detail
