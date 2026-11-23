#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "borrowed_text.h"
#include "http_message.h"
#include "ip_address.h"

namespace ruvia {

// Numeric IP host syntax without URI brackets.
[[nodiscard]] bool is_valid_http_ipv4_literal(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_http_ipv6_literal(std::string_view value) noexcept;

namespace detail {
struct http_authority_view_access;
}

enum class http_authority_port_kind : std::uint8_t {
    absent,
    empty,
    value,
};

enum class http_host_kind : std::uint8_t {
    reg_name,
    ipv4,
    ipv6,
    ipv_future,
};

// RFC reception preserves IPvFuture, explicit zero, empty ports and leading
// zeros. Endpoint configuration additionally rejects those four states.
// Neither policy admits userinfo, zones or a port outside 0..65535.
enum class http_authority_policy : std::uint8_t {
    rfc,
    endpoint,
};

// A validated, borrowed RFC 3986 authority without userinfo; IP-literal hosts
// keep their brackets. An explicit empty port stays distinct from an absent one
// for syntax-preserving parsing; both map to the scheme default when an HTTP
// origin is compared.
class http_authority_view final {
public:
    [[nodiscard]] constexpr std::string_view host() const noexcept {
        return host_;
    }

    [[nodiscard]] constexpr http_host_kind host_kind() const noexcept {
        return host_kind_;
    }

    // Original digits, including leading zeros. port_kind() distinguishes an
    // absent delimiter from an explicitly empty port.
    [[nodiscard]] constexpr std::string_view port_text() const noexcept {
        return port_text_;
    }

    [[nodiscard]] constexpr http_authority_port_kind port_kind() const noexcept {
        return port_kind_;
    }

    [[nodiscard]] constexpr std::optional<port_number> port() const noexcept {
        return port_kind_ == http_authority_port_kind::value ? std::optional<port_number>(port_number(port_)) : std::nullopt;
    }

    // Returns the explicit port or caller fallback. This numeric observation
    // alone does not preserve the absent/empty versus explicit-zero distinction.
    [[nodiscard]] constexpr port_number effective_port(port_number default_port) const noexcept {
        return port_kind_ == http_authority_port_kind::value ? port_number(port_) : default_port;
    }

private:
    friend struct detail::http_authority_view_access;

    constexpr http_authority_view(std::string_view host, http_host_kind host_kind, std::string_view port_text, http_authority_port_kind port_kind, std::uint16_t port) noexcept :
        host_(host),
        port_text_(port_text),
        port_(port),
        port_kind_(port_kind),
        host_kind_(host_kind) {}

    std::string_view host_;
    std::string_view port_text_;
    std::uint16_t port_{0};
    http_authority_port_kind port_kind_{http_authority_port_kind::absent};
    http_host_kind host_kind_{http_host_kind::reg_name};
};

[[nodiscard]] std::optional<http_authority_view> parse_http_authority(borrowed_text value RUVIA_LIFETIMEBOUND, http_authority_policy policy = http_authority_policy::rfc) noexcept;

// Parses a Host-field authority without userinfo and borrows its host text.
// The host retains brackets around an IP literal. An invalid authority has no
// value; a syntactically valid empty host remains an engaged empty view.
[[nodiscard]] std::optional<std::string_view> parse_http_authority_host(borrowed_text value RUVIA_LIFETIMEBOUND) noexcept;

// Compares HTTP authorities using case-insensitive host syntax and the supplied
// scheme default port, checked before narrowing like other port inputs.
// Without a scheme default (std::nullopt) an absent/empty port has no numeric
// value: it equals only another absent/empty port, never an explicit one.
// Invalid authorities never compare equal.
[[nodiscard]] bool http_authorities_equal(borrowed_text left, borrowed_text right, std::optional<port_number> default_port) noexcept;

// Validates a WHATWG serialized origin, not a general URI or origin list.
[[nodiscard]] bool is_valid_http_serialized_origin(std::string_view value) noexcept;

struct http_origin_options final {
    borrowed_text host_{};
    std::optional<port_number> port_{};
};

class http_origin_view final {
public:
    // `host` is a borrowed RFC 3986 uri-host; its storage must remain alive and
    // unchanged whenever this value is consumed. IP literals therefore include
    // brackets (for example, "[::1]"). Factories reject an empty or malformed
    // host before an origin can be observed. An absent port resolves to the
    // scheme default.
    http_origin_view(http_scheme scheme, http_origin_options options);

    [[nodiscard]] static http_origin_view http(http_origin_options options) {
        return http_origin_view(http_scheme::http, options);
    }

    [[nodiscard]] static http_origin_view https(http_origin_options options) {
        return http_origin_view(http_scheme::https, options);
    }

    [[nodiscard]] constexpr http_scheme scheme() const noexcept {
        return scheme_;
    }

    // RFC 3986 uri-host only; keep the port in port().
    [[nodiscard]] constexpr std::string_view host() const noexcept {
        return host_;
    }

    [[nodiscard]] constexpr port_number port() const noexcept {
        return port_;
    }

private:
    std::string_view host_;
    port_number port_;
    http_scheme scheme_;
};

// RFC 3986 authority for this origin: preserves IP-literal brackets and
// includes the port only when it differs from the scheme's default. The
// returned string owns its bytes; the origin remains borrowed.
[[nodiscard]] std::string format_http_origin_authority(const http_origin_view& origin);

}  // namespace ruvia
