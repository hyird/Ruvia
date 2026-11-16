#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "borrowed_text.h"
#include "http_authority.h"
#include "http_message.h"

namespace ruvia {

// ASCII LDH labels, 1..63 bytes each, total at most 253 bytes. No trailing
// root dot, empty label, Unicode, percent escapes or leading/trailing hyphen.
// IDNA conversion and public-suffix policy belong to the caller.
[[nodiscard]] bool is_valid_dns_domain_name(std::string_view name) noexcept;

// Proper subdomain at a label boundary, ASCII case-insensitive. Inputs are
// already validated DNS names; equality is not a proper subdomain.
[[nodiscard]] constexpr bool is_dns_subdomain_of(std::string_view host, std::string_view domain) noexcept {
    return host.size() > domain.size() && host[host.size() - domain.size() - 1] == '.' && http_ascii_ends_with_ignore_case(host, domain);
}

// Split RFC 3986 host [ ":" port ], preserving IP-literal brackets. This only
// checks delimiters, not host/port syntax. Rejects unbalanced brackets, bytes
// after ']' other than ':', and colons in port. Outputs are unspecified on
// failure and otherwise borrow value; an empty host/port is representable.
[[nodiscard]] bool split_uri_host_port(std::string_view value, std::string_view& host, std::string_view& port, bool& has_port) noexcept;
template <detail::http_temporary_owning_char_string text_type>
bool split_uri_host_port(text_type&&, std::string_view&, std::string_view&, bool&) = delete;

// A complete HTTP(S)/WS(S) URL, without userinfo or IPv6 zones. All non-default
// text borrows the input without decoding or normalization. Scheme case and
// authority spelling (including port digits) are retained. Fragments are
// validated and exposed, but never included in the origin-form target.
class http_url_view final {
public:
    [[nodiscard]] constexpr std::string_view scheme() const noexcept {
        return scheme_;
    }
    [[nodiscard]] constexpr std::string_view authority_text() const noexcept {
        return authority_text_;
    }
    [[nodiscard]] constexpr const http_authority_view& authority() const& noexcept RUVIA_LIFETIMEBOUND {
        return authority_;
    }
    const http_authority_view& authority() const&& = delete;
    [[nodiscard]] constexpr std::string_view path() const noexcept {
        return path_.empty() ? std::string_view("/") : path_;
    }
    [[nodiscard]] constexpr bool has_path() const noexcept {
        return !path_.empty();
    }
    // nullopt is absent; an engaged empty view represents a trailing '?'/'#'.
    [[nodiscard]] constexpr std::optional<std::string_view> query() const noexcept {
        return query_;
    }
    [[nodiscard]] constexpr std::optional<std::string_view> fragment() const noexcept {
        return fragment_;
    }

    // Concatenating these two slices gives the origin-form target. A missing
    // path uses static "/"; only a query-only URL needs the second slice.
    // This avoids allocating "/?query" when no such contiguous input exists.
    [[nodiscard]] constexpr std::array<std::string_view, 2> target() const noexcept {
        return path_.empty() ? std::array<std::string_view, 2>{"/", raw_target_} : std::array<std::string_view, 2>{raw_target_, {}};
    }

private:
    friend std::optional<http_url_view> parse_http_url(borrowed_text, http_authority_policy) noexcept;
    constexpr http_url_view(std::string_view scheme, std::string_view authority_text, http_authority_view authority, std::string_view path, std::optional<std::string_view> query, std::optional<std::string_view> fragment, std::string_view raw_target) noexcept :
        scheme_(scheme),
        authority_text_(authority_text),
        authority_(authority),
        path_(path),
        query_(query),
        fragment_(fragment),
        raw_target_(raw_target) {}

    std::string_view scheme_;
    std::string_view authority_text_;
    http_authority_view authority_;
    std::string_view path_;
    std::optional<std::string_view> query_;
    std::optional<std::string_view> fragment_;
    std::string_view raw_target_;
};

// ASCII RFC 3986 components; rejects malformed escapes, raw whitespace,
// backslashes and encoded NUL in the path. No dot-segment normalization.
// RFC-valid receiver authority spellings remain accepted by default; select
// endpoint explicitly for endpoint configuration, not generic HTTP reception.
[[nodiscard]] std::optional<http_url_view> parse_http_url(borrowed_text value, http_authority_policy policy = http_authority_policy::rfc) noexcept;

}  // namespace ruvia
