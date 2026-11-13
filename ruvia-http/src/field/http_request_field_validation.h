#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_target.h"

#include "field/http_connection_fields.h"
#include "field/http_field_kind.h"
#include "memory/geometric_reserve.h"
#include "parser/http_request_target.h"
namespace ruvia::detail {

// The standardized Expect field member is defined once so parsers and writers
// cannot drift on its wire spelling.
inline constexpr std::string_view http_continue_expectation_token = "100-continue";

// RFC 6454 section 7.1 permits either `null` or a space-delimited list of
// serialized origins. Fetch-generated CORS requests currently send one item,
// but the HTTP protocol primitive must retain the complete field grammar.
[[nodiscard]] inline bool is_valid_http_origin_field_value(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (value == "null") {
        return true;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto separator = value.find(' ', offset);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        if (!is_valid_http_serialized_origin(value.substr(offset, end - offset))) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
    }
}

inline constexpr auto binary_field_name_char_table = [] {
    auto table_value = http_token_char_table;
    for (unsigned byte = 'A'; byte <= 'Z'; ++byte) {
        table_value[byte] = false;
    }
    return table_value;
}();

// HTTP/2 (RFC 9113 §8.2.1) and HTTP/3 (RFC 9114 §4.2) regular field names are
// nonempty lowercase tokens.
[[nodiscard]] inline bool is_valid_binary_field_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    std::size_t index = 0;
    while (name.size() - index >= 4) {
        const auto* bytes_value = reinterpret_cast<const unsigned char*>(name.data() + index);
        const bool first = binary_field_name_char_table[bytes_value[0]];
        const bool second = binary_field_name_char_table[bytes_value[1]];
        const bool third = binary_field_name_char_table[bytes_value[2]];
        const bool fourth = binary_field_name_char_table[bytes_value[3]];
        if (!(first & second & third & fourth)) {
            return false;
        }
        index += 4;
    }
    while (index < name.size()) {
        if (!binary_field_name_char_table[static_cast<unsigned char>(name[index])]) {
            return false;
        }
        ++index;
    }
    return true;
}

[[nodiscard]] inline bool is_http_request_scheme(std::string_view scheme) noexcept {
    return http_ascii_equals_ignore_case(scheme, "http") || http_ascii_equals_ignore_case(scheme, "https");
}

// http(s) target URIs carry a mandatory, non-empty authority with Host
// syntax; other schemes may carry any RFC 3986 authority.
[[nodiscard]] inline bool is_valid_binary_request_authority(
    std::string_view scheme, std::string_view authority) noexcept {
    if (is_http_request_scheme(scheme)) {
        return !authority.empty() && is_valid_host_header(authority);
    }
    return is_valid_uri_authority(authority);
}

[[nodiscard]] inline bool binary_regular_request_requires_authority(
    std::string_view scheme, std::string_view path) noexcept {
    return path != "*" && is_http_request_scheme(scheme);
}

// HTTP(S) already uses Host syntax; other schemes can contain URI userinfo.
[[nodiscard]] inline bool binary_request_synthesizes_host(bool has_host, bool has_authority,
    std::string_view scheme, std::string_view authority) noexcept {
    return !has_host && has_authority &&
           (is_http_request_scheme(scheme) || is_valid_host_header(authority));
}

// RFC 8441 §4 / RFC 9220 §3: websocket extended CONNECT requires an HTTP(S) URI
// scheme; other extended protocols retain the full RFC 3986 space.
[[nodiscard]] inline bool is_valid_extended_connect_scheme(
    std::string_view protocol, std::string_view scheme) noexcept {
    return !http_ascii_equals_ignore_case(protocol, "websocket") || is_http_request_scheme(scheme);
}

[[nodiscard]] inline bool validate_binary_request_authority_and_host(
    std::string_view scheme, std::string_view path, bool has_authority, std::string_view authority,
    bool has_host, std::string_view host) noexcept {
    if (has_authority && !is_valid_binary_request_authority(scheme, authority)) {
        return false;
    }
    if (!has_authority && binary_regular_request_requires_authority(scheme, path) &&
        (!has_host || !is_valid_binary_request_authority(scheme, host))) {
        return false;
    }
    return !has_authority || !has_host ||
           authority_matches_host(authority, host, http_uri_scheme_default_port(scheme));
}

// Joins split binary (RFC 9113 §8.2.3, RFC 9114 §4.2.1) Cookie crumbs with "; "
// within max_http_header_bytes. When `first_crumb` is non-empty and `joined` is
// still empty, `first_crumb` is materialized before appending the second crumb.
[[nodiscard]] inline bool append_binary_cookie_crumb(std::string& joined,
    std::string_view value, bool has_existing_cookie, std::string_view first_crumb = {}) {
    if (value.empty()) {
        return true;
    }
    if (value.size() > max_http_header_bytes) {
        return false;
    }
    if (!has_existing_cookie) {
        reserve_geometric(joined, value.size());
        joined.append(value.data(), value.size());
        return true;
    }
    constexpr std::string_view cookie_separator = "; ";
    const auto prefix_bytes = joined.empty() ? first_crumb.size() : joined.size();
    if (prefix_bytes > max_http_header_bytes - cookie_separator.size() ||
        value.size() > max_http_header_bytes - cookie_separator.size() - prefix_bytes) {
        return false;
    }
    reserve_geometric(joined, prefix_bytes + cookie_separator.size() + value.size());
    if (joined.empty() && !first_crumb.empty()) {
        joined.append(first_crumb.data(), first_crumb.size());
    }
    joined.append(cookie_separator.data(), cookie_separator.size());
    joined.append(value.data(), value.size());
    return true;
}

[[nodiscard]] constexpr bool is_valid_http_request_field_source(http_request_field_source source) noexcept {
    return source == http_request_field_source::generated || source == http_request_field_source::forwarded;
}

// The only version-dependent request-field semantics: HTTP/1.1 negotiates
// transfer codings through TE, while HTTP/2 permits only "trailers".
enum class request_field_framing : std::uint8_t { http1,
    binary };

enum class request_field_status : std::uint8_t {
    ok,
    // Grammar violation, or a second instance of a field that may occur once.
    invalid,
    // A client generated the field itself. Expect and Content-Length are
    // derived from the request plan on every HTTP version, so a raw field
    // would be a second source of truth for the body gate or the framing.
    managed_by_sender,
};

// Per field section: which once-only fields were seen, plus the framing whose
// TE rules apply. One instance spans exactly one request header section.
class request_field_state final {
public:
    explicit request_field_state(request_field_framing framing) noexcept
        : framing_(framing) {}

    // True once a field that may occur only once (Host and every
    // singleton_request_header_bit() kind) has been validated.
    [[nodiscard]] bool seen(http_field_kind kind) const noexcept;

private:
    friend request_field_status validate_request_field(http_field_kind kind, std::string_view value,
        http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept;

    std::uint32_t seen_{0};
    request_field_framing framing_;
};

// The authoritative semantic check for one request field, shared by the
// HTTP/1 and HTTP/2 request writers (role sender) and request
// recipients (role recipient). Callers classify the name once with
// classify_http_field() and keep version-specific concerns themselves:
// framing state (Content-Length values on receipt, Transfer-Encoding,
// Connection, Upgrade), Host/:authority agreement, cookie joining, and parsing
// accepted Expect members.
[[nodiscard]] request_field_status validate_request_field(http_field_kind kind, std::string_view value,
    http_field_list_role role, request_field_state& state,
    http_request_field_source source = http_request_field_source::generated) noexcept;

[[nodiscard]] bool is_protocol_managed_request_field(
    http_field_kind kind, std::string_view name) noexcept;
}  // namespace ruvia::detail
