#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"

namespace ruvia::detail {

struct request_target_view {
    std::string_view scheme_;
    std::string_view path_;
    std::string_view query_;
    std::string_view authority_;
    http_request_target_form form_{http_request_target_form::origin};
};

// Validates the byte repertoire shared by the four HTTP request-target forms.
// Component-specific rules (for example, '[' and ']' being legal only in an
// IP-literal authority) are applied by parse_request_target /
// is_valid_origin_form_target.
[[nodiscard]] bool is_valid_request_target_bytes(std::string_view target) noexcept;

// RFC 3986 scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ).
[[nodiscard]] bool is_valid_uri_scheme(std::string_view value) noexcept;

// Strict origin-form starts with '/'. Asterisk-form is a separate target form
// and is exposed explicitly so callers cannot lose its OPTIONS-only contract.
[[nodiscard]] bool is_valid_origin_form_target(std::string_view target) noexcept;
[[nodiscard]] bool is_valid_origin_or_asterisk_form_target(
    http_known_method method, std::string_view target) noexcept;

// Host field syntax. An empty field is valid when the target URI has no
// authority (RFC 9112 section 3.2); callers that require a usable HTTP origin
// must additionally require a non-empty value.
[[nodiscard]] bool is_valid_host_header(std::string_view value) noexcept;
// RFC 3986 `host` / HTTP `uri-host`, without a port. IP literals use their
// standard bracketed form; IPv6 zone identifiers are not URI syntax (RFC 9844
// reverted RFC 6874), while IPvFuture remains valid.
[[nodiscard]] bool is_valid_http_host(std::string_view value) noexcept;
[[nodiscard]] bool http_uri_host_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool parse_request_target(
    http_known_method method, std::string_view target, request_target_view& output) noexcept;
template <http_temporary_owning_char_string target_type>
bool parse_request_target(http_known_method, target_type&&, request_target_view&) = delete;
[[nodiscard]] bool authority_matches_host(
    std::string_view authority, std::string_view host, std::uint16_t default_port) noexcept;

}  // namespace ruvia::detail
