#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http_known_method.h"

namespace ruvia::detail {

enum class http_request_target_form : std::uint8_t {
    origin,
    absolute,
    authority,
    asterisk,
};

struct request_target_view {
    std::string_view scheme_;
    std::string_view path_;
    std::string_view query_;
    std::string_view authority_;
    std::uint16_t default_port_{0};
    http_request_target_form form_{http_request_target_form::origin};
};

enum class http_authority_port_kind : std::uint8_t {
    absent,
    empty,
    value,
};

struct http_authority_view_access;

// A validated RFC 3986 authority without userinfo. Keeping an explicit empty
// port distinct from an absent port is required for syntax-preserving parsing;
// both map to the scheme default when an HTTP origin is compared.
class http_authority_view final {
public:
    [[nodiscard]] constexpr std::string_view host() const noexcept {
        return host_;
    }

    [[nodiscard]] constexpr http_authority_port_kind port_kind() const noexcept {
        return port_kind_;
    }

    [[nodiscard]] constexpr std::optional<std::uint16_t> port() const noexcept {
        return port_kind_ == http_authority_port_kind::value ? std::optional<std::uint16_t>(port_)
                                                             : std::nullopt;
    }

    [[nodiscard]] constexpr std::uint16_t effective_port(std::uint16_t default_port) const noexcept {
        return port_kind_ == http_authority_port_kind::value ? port_ : default_port;
    }

private:
    friend struct http_authority_view_access;

    constexpr http_authority_view(
        std::string_view host, http_authority_port_kind port_kind, std::uint16_t port) noexcept
        : host_(host),
          port_(port),
          port_kind_(port_kind) {}

    std::string_view host_;
    std::uint16_t port_{0};
    http_authority_port_kind port_kind_{http_authority_port_kind::absent};
};

// Validates the byte repertoire shared by the four HTTP request-target forms.
// Component-specific rules (for example, '[' and ']' being legal only in an
// IP-literal authority) are applied by parse_request_target /
// is_valid_origin_form_target.
[[nodiscard]] bool is_valid_request_target_bytes(std::string_view target) noexcept;

// RFC 3986 scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ).
[[nodiscard]] bool is_valid_uri_scheme(std::string_view value) noexcept;
// Returns the HTTP-defined default for http/https (case-insensitively), or 0
// when the framework does not know a default port for the valid URI scheme.
[[nodiscard]] std::uint16_t http_uri_scheme_default_port(std::string_view scheme) noexcept;

// Strict origin-form starts with '/'. Asterisk-form is a separate target form
// and is exposed explicitly so callers cannot lose its OPTIONS-only contract.
[[nodiscard]] bool is_valid_origin_form_target(std::string_view target) noexcept;
[[nodiscard]] bool is_valid_origin_or_asterisk_form_target(std::string_view target) noexcept;
[[nodiscard]] bool is_valid_origin_or_asterisk_form_target(
    http_known_method method, std::string_view target) noexcept;

// Host field syntax. An empty field is valid when the target URI has no
// authority (RFC 9112 section 3.2); callers that require a usable HTTP origin
// must additionally require a non-empty value.
[[nodiscard]] bool is_valid_host_header(std::string_view value) noexcept;
// RFC 3986 authority = [ userinfo "@" ] host [ ":" port ]. Unlike an HTTP
// Host field, the generic grammar permits userinfo, an empty reg-name, and a
// syntactically unbounded decimal port. Scheme-specific rules remain separate.
[[nodiscard]] bool is_valid_uri_authority(std::string_view value) noexcept;
// RFC 3986 `host` / HTTP `uri-host`, without a port. IP literals use their
// standard bracketed form; IPv6 zone identifiers are not URI syntax (RFC 9844
// reverted RFC 6874), while IPvFuture remains valid.
[[nodiscard]] bool is_valid_http_host(std::string_view value) noexcept;
[[nodiscard]] std::optional<http_authority_view> parse_http_authority(std::string_view value) noexcept;
template <http_temporary_owning_char_string value_type>
std::optional<http_authority_view> parse_http_authority(value_type&&) = delete;
[[nodiscard]] bool http_uri_host_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool parse_request_target(
    http_known_method method, std::string_view target, request_target_view& output) noexcept;
template <http_temporary_owning_char_string target_type>
bool parse_request_target(http_known_method, target_type&&, request_target_view&) = delete;
[[nodiscard]] bool authority_matches_host(
    std::string_view authority, std::string_view host, std::uint16_t default_port) noexcept;

}  // namespace ruvia::detail
