#include "parser/http_request_target.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <system_error>
#include <variant>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/hex.h"

#include "parser/http_uri_grammar.h"

namespace ruvia::detail {
namespace {

// URI reg-name chars: unreserved / sub-delims (pct-encoded handled by caller).

struct normalized_host_unit final {
    unsigned char byte_{0};
    bool encoded_reserved_{false};
};

[[nodiscard]] bool next_normalized_host_unit(
    std::string_view value, std::size_t& cursor_value, normalized_host_unit& output) noexcept {
    if (cursor_value == value.size()) {
        return false;
    }

    auto byte = static_cast<unsigned char>(value[cursor_value++]);
    bool encoded_reserved = false;
    if (byte == '%' && cursor_value + 1 < value.size()) {
        const auto high = decode_hex_nibble(value[cursor_value]);
        const auto low = decode_hex_nibble(value[cursor_value + 1]);
        if (high >= 0 && low >= 0) {
            byte = static_cast<unsigned char>((high << 4) | low);
            cursor_value += 2;
            // RFC 3986 sections 2.2, 2.3, and 6.2.2.2: percent-encoded
            // unreserved octets are equivalent to their decoded spelling,
            // but an encoded reserved octet is not equivalent to the raw
            // reserved character. Preserve that distinction while still
            // normalizing the case of the hexadecimal spelling.
            encoded_reserved = !is_unreserved_byte(byte);
        }
    }
    if (!encoded_reserved && byte >= 'A' && byte <= 'Z') {
        byte = static_cast<unsigned char>(byte + ('a' - 'A'));
    }
    output = normalized_host_unit{.byte_ = byte, .encoded_reserved_ = encoded_reserved};
    return true;
}

[[nodiscard]] bool is_valid_connect_authority_form(std::string_view target) noexcept {
    const auto authority = parse_http_authority(target);
    if (!authority || authority->port_kind() != http_authority_port_kind::value) {
        return false;
    }
    // RFC 9110 section 9.3.6 requires a non-empty, valid tunnel destination
    // port. Port zero is reserved and cannot identify that destination.
    return *authority->port() != 0;
}

}  // namespace

struct http_authority_view_access final {
    [[nodiscard]] static constexpr http_authority_view make(
        std::string_view host, http_authority_port_kind port_kind, std::uint16_t port = 0) noexcept {
        return http_authority_view(host, port_kind, port);
    }
};

bool is_valid_request_target_bytes(std::string_view target) noexcept {
    if (target.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < target.size(); ++i) {
        const auto byte = static_cast<unsigned char>(target[i]);
        if (byte == '%') {
            if (i + 2 >= target.size() || decode_hex_nibble(target[i + 1]) < 0 ||
                decode_hex_nibble(target[i + 2]) < 0) {
                return false;
            }
            i += 2;
            continue;
        }
        // RFC 3986 URI-reference is ASCII and consists only of unreserved or
        // reserved characters. A fragment delimiter is never part of an HTTP
        // request target. Brackets are admitted here because this low-level
        // union also covers an IP-literal authority; component validation below
        // rejects them from path and query.
        if (!is_uri_pchar(byte) && byte != '/' && byte != '?' && byte != '[' && byte != ']') {
            return false;
        }
    }
    return true;
}

bool is_valid_origin_form_target(std::string_view target) noexcept {
    if (target.empty() || target.front() != '/') {
        return false;
    }
    const auto separator = target.find('?');
    const auto path = separator == std::string_view::npos ? target : target.substr(0, separator);
    const auto query =
        separator == std::string_view::npos ? std::string_view{} : target.substr(separator + 1);
    return is_valid_uri_component(path, true, false) && is_valid_uri_component(query, true, true);
}

bool is_valid_origin_or_asterisk_form_target(std::string_view target) noexcept {
    return target == "*" || is_valid_origin_form_target(target);
}

bool is_valid_origin_or_asterisk_form_target(http_known_method method, std::string_view target) noexcept {
    return target == "*" ? method == http_known_method::options : is_valid_origin_form_target(target);
}

bool is_valid_http_host(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() == '[') {
        if (value.size() < 3 || value.back() != ']') {
            return false;
        }
        const auto literal = value.substr(1, value.size() - 2);
        return is_valid_ipv6_literal(literal) || is_valid_ipv_future(literal);
    }
    return !(value.find(':') != std::string_view::npos) && is_valid_reg_name(value);
}

std::optional<http_authority_view> parse_http_authority(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }

    std::string_view host;
    std::string_view port_text;
    bool has_port_delimiter = false;
    if (value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = value.substr(0, close + 1);
        const auto remainder = value.substr(close + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':') {
                return std::nullopt;
            }
            has_port_delimiter = true;
            port_text = remainder.substr(1);
        }
    } else {
        const auto colon = value.find(':');
        host = colon == std::string_view::npos ? value : value.substr(0, colon);
        if (colon != std::string_view::npos) {
            has_port_delimiter = true;
            port_text = value.substr(colon + 1);
        }
    }

    if (!is_valid_http_host(host)) {
        return std::nullopt;
    }
    if (!has_port_delimiter) {
        return http_authority_view_access::make(host, http_authority_port_kind::absent);
    }
    if (port_text.empty()) {
        return http_authority_view_access::make(host, http_authority_port_kind::empty);
    }

    const auto port = parse_port_value(port_text);
    if ((port.index() != 0)) {
        return std::nullopt;
    }
    return http_authority_view_access::make(host, http_authority_port_kind::value, std::get<0>(port));
}

bool http_uri_host_equals(std::string_view left, std::string_view right) noexcept {
    std::size_t left_cursor = 0;
    std::size_t right_cursor = 0;
    for (;;) {
        normalized_host_unit left_unit;
        normalized_host_unit right_unit;
        const auto has_left = next_normalized_host_unit(left, left_cursor, left_unit);
        const auto has_right = next_normalized_host_unit(right, right_cursor, right_unit);
        if (!has_left || !has_right) {
            return has_left == has_right;
        }
        if (left_unit.byte_ != right_unit.byte_ ||
            left_unit.encoded_reserved_ != right_unit.encoded_reserved_) {
            return false;
        }
    }
}

bool is_valid_host_header(std::string_view value) noexcept {
    // RFC 9112 section 3.2 requires an empty Host field when the target URI
    // has no authority component. Keep that wire-valid state distinct from a
    // usable network authority: parse_http_authority() intentionally continues
    // to require a host.
    return value.empty() || parse_http_authority(value).has_value();
}

bool is_valid_uri_authority(std::string_view value) noexcept {
    auto host_and_port = value;
    if (const auto delimiter = value.find('@'); delimiter != std::string_view::npos) {
        if (!is_valid_uri_userinfo(value.substr(0, delimiter)) ||
            value.find('@', delimiter + 1) != std::string_view::npos) {
            return false;
        }
        host_and_port = value.substr(delimiter + 1);
    }

    std::string_view host;
    std::string_view port;
    bool has_port = false;
    if (!host_and_port.empty() && host_and_port.front() == '[') {
        const auto close = host_and_port.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        host = host_and_port.substr(0, close + 1);
        const auto remainder = host_and_port.substr(close + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':') {
                return false;
            }
            has_port = true;
            port = remainder.substr(1);
        }
    } else {
        const auto delimiter = host_and_port.find(':');
        host = delimiter == std::string_view::npos ? host_and_port : host_and_port.substr(0, delimiter);
        if (delimiter != std::string_view::npos) {
            has_port = true;
            port = host_and_port.substr(delimiter + 1);
            if ((port.find(':') != std::string_view::npos)) {
                return false;
            }
        }
    }

    return (host.empty() || is_valid_http_host(host)) && (!has_port || is_valid_uri_port(port));
}

bool is_valid_uri_scheme(std::string_view value) noexcept {
    const auto is_alpha = [](unsigned char byte) noexcept {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
    };
    if (value.empty() || !is_alpha(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (const auto c : value.substr(1)) {
        const auto byte = static_cast<unsigned char>(c);
        if (!is_alpha(byte) && !(byte >= '0' && byte <= '9') && byte != '+' && byte != '-' &&
            byte != '.') {
            return false;
        }
    }
    return true;
}

std::uint16_t http_uri_scheme_default_port(std::string_view scheme) noexcept {
    if (http_ascii_equals_ignore_case(scheme, "http")) {
        return 80;
    }
    if (http_ascii_equals_ignore_case(scheme, "https")) {
        return 443;
    }
    return 0;
}

namespace {

[[nodiscard]] bool parse_absolute_target(
    http_known_method method, std::string_view target, request_target_view& output) noexcept {
    // RFC 9112 section 3.2.2 defines absolute-form as the complete RFC 3986
    // absolute-URI grammar. Restricting this to HTTP(S) rejects valid proxy
    // requests such as ftp:// targets and every authority-less scheme.
    const auto scheme_end = target.find(':');
    if (scheme_end == std::string_view::npos || !is_valid_uri_scheme(target.substr(0, scheme_end))) {
        return false;
    }

    const auto scheme = target.substr(0, scheme_end);
    const bool http_scheme_value =
        http_ascii_equals_ignore_case(scheme, "http") || http_ascii_equals_ignore_case(scheme, "https");
    const auto remainder = target.substr(scheme_end + 1);
    const auto query_separator = remainder.find('?');
    const auto hierarchy =
        query_separator == std::string_view::npos ? remainder : remainder.substr(0, query_separator);
    const auto query = query_separator == std::string_view::npos
                           ? std::string_view{}
                           : remainder.substr(query_separator + 1);
    if (!is_valid_uri_component(query, true, true)) {
        return false;
    }

    std::string_view authority;
    std::string_view path;
    if (hierarchy.starts_with("//")) {
        const auto authority_and_path = hierarchy.substr(2);
        const auto path_separator = authority_and_path.find('/');
        const auto uri_authority = path_separator == std::string_view::npos
                                       ? authority_and_path
                                       : authority_and_path.substr(0, path_separator);
        path = path_separator == std::string_view::npos ? std::string_view{}
                                                        : authority_and_path.substr(path_separator);
        if (!is_valid_uri_authority(uri_authority) || !is_valid_uri_component(path, true, false)) {
            return false;
        }

        const auto userinfo_delimiter = uri_authority.find('@');
        authority = userinfo_delimiter == std::string_view::npos
                        ? uri_authority
                        : uri_authority.substr(userinfo_delimiter + 1);
        // Host is the public, authoritative routing value installed by the
        // HTTP/1 parser. It therefore still has to fit the validated HTTP
        // authority representation even when the URI scheme is generic.
        if (!is_valid_host_header(authority)) {
            return false;
        }

        // HTTP(S) URI syntax has a mandatory, non-empty authority. Userinfo in
        // an HTTP URI is rejected at this trust boundary (RFC 9110 section
        // 4.2.4) instead of being silently stripped into a routing identity.
        if (http_scheme_value && (authority.empty() || userinfo_delimiter != std::string_view::npos)) {
            return false;
        }
    } else {
        // RFC 9110 defines HTTP(S) URI with "//" authority. Other registered
        // schemes may use path-absolute, path-rootless, or path-empty.
        if (http_scheme_value || !is_valid_uri_component(hierarchy, true, false)) {
            return false;
        }
        path = hierarchy;
    }

    if (path.empty() && method == http_known_method::options && query_separator == std::string_view::npos) {
        // RFC 9112 section 3.2.4: a proxy forwarding an absolute-form
        // OPTIONS target with an empty path and no query to the final origin
        // must use asterisk-form. Expose that route semantic directly even
        // when this parser itself is the origin-facing recipient.
        output.path_ = "*";
    } else if (path.empty() && http_scheme_value) {
        // RFC 9110 section 4.2.3 permits this normalization only for HTTP(S)
        // targets. Generic schemes retain their exact empty path, while an
        // OPTIONS target with a query remains distinct from server-wide "*".
        output.path_ = "/";
    } else {
        output.path_ = path;
    }
    output.scheme_ = scheme;
    output.query_ = query;
    output.authority_ = authority;
    output.default_port_ = http_uri_scheme_default_port(scheme);
    output.form_ = http_request_target_form::absolute;
    return true;
}

}  // namespace

bool authority_matches_host(
    std::string_view authority, std::string_view host, std::uint16_t default_port) noexcept {
    const auto authority_parts = parse_http_authority(authority);
    const auto host_parts = parse_http_authority(host);
    if (!authority_parts || !host_parts ||
        !http_uri_host_equals(authority_parts->host(), host_parts->host())) {
        return false;
    }
    // With no known scheme default, an omitted/empty port has no numeric value.
    // In particular, it must not compare equal to the explicit port `:0` merely
    // because zero is also our "unknown default" sentinel.
    if (default_port == 0) {
        const bool authority_has_port = authority_parts->port_kind() == http_authority_port_kind::value;
        const bool host_has_port = host_parts->port_kind() == http_authority_port_kind::value;
        if (authority_has_port != host_has_port) {
            return false;
        }
        return !authority_has_port || authority_parts->port() == host_parts->port();
    }
    return authority_parts->effective_port(default_port) == host_parts->effective_port(default_port);
}

bool parse_request_target(
    http_known_method method, std::string_view target, request_target_view& output) noexcept {
    if (target == "*") {
        if (method != http_known_method::options) {
            return false;
        }
        output.path_ = "*";
        output.scheme_ = {};
        output.query_ = {};
        output.authority_ = {};
        output.default_port_ = 0;
        output.form_ = http_request_target_form::asterisk;
        return true;
    }
    if (method == http_known_method::connect) {
        if (!is_valid_connect_authority_form(target)) {
            return false;
        }
        output.path_ = target;
        output.scheme_ = {};
        output.query_ = {};
        output.authority_ = target;
        output.default_port_ = 0;
        output.form_ = http_request_target_form::authority;
        return true;
    }
    if (target.empty()) {
        return false;
    }
    if (target.front() == '/') {
        if (!is_valid_origin_form_target(target)) {
            return false;
        }
        const auto query_separator = target.find('?');
        output.scheme_ = {};
        output.path_ =
            query_separator == std::string_view::npos ? target : target.substr(0, query_separator);
        output.query_ = query_separator == std::string_view::npos ? std::string_view{}
                                                                  : target.substr(query_separator + 1);
        output.authority_ = {};
        output.default_port_ = 0;
        output.form_ = http_request_target_form::origin;
        return !output.path_.empty();
    }
    if (!is_valid_request_target_bytes(target)) {
        return false;
    }
    return parse_absolute_target(method, target, output);
}

}  // namespace ruvia::detail
