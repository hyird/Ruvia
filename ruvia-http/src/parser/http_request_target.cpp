#include "parser/http_request_target.h"

#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/http/url_encoding.h"

#include "parser/http_uri_grammar.h"

namespace ruvia::detail {
namespace {

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
    if (byte == '%') {
        if (const auto decoded = http_try_decode_percent_byte(value, cursor_value - 1)) {
            byte = *decoded;
            cursor_value += 2;
            // RFC 3986 sections 2.2, 2.3, and 6.2.2.2: percent-encoded
            // unreserved octets are equivalent to their decoded spelling,
            // but an encoded reserved octet is not equivalent to the raw
            // reserved character. Preserve that distinction while still
            // normalizing the case of the hexadecimal spelling.
            encoded_reserved = !is_unreserved_byte(byte);
        }
    }
    if (!encoded_reserved) {
        byte = http_ascii_to_lower(byte);
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

// Splits origin-form = absolute-path [ "?" query ] and validates both parts.
[[nodiscard]] bool split_origin_form_target(
    std::string_view target, std::string_view& path, std::string_view& query) noexcept {
    if (target.empty() || target.front() != '/') {
        return false;
    }
    const auto separator = target.find('?');
    path = separator == std::string_view::npos ? target : target.substr(0, separator);
    query = separator == std::string_view::npos ? std::string_view{} : target.substr(separator + 1);
    return is_valid_uri_component(path, true, false) && is_valid_uri_component(query, true, true);
}

}  // namespace

bool is_valid_request_target_bytes(std::string_view target) noexcept {
    if (target.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < target.size(); ++i) {
        const auto byte = static_cast<unsigned char>(target[i]);
        if (byte == '%') {
            if (!http_try_decode_percent_byte(target, i)) {
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
    std::string_view path;
    std::string_view query;
    return split_origin_form_target(target, path, query);
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
        return parse_ipv6_address(literal).has_value() || is_valid_ipv_future(literal);
    }
    return !(value.find(':') != std::string_view::npos) && is_valid_reg_name(value);
}

bool http_uri_host_equals(std::string_view left, std::string_view right) noexcept {
    if (left == right) {
        return true;
    }
    if (left.size() > 2 && right.size() > 2 && left.front() == '[' && right.front() == '[') {
        const auto left_address = parse_ipv6_address(left.substr(1, left.size() - 2));
        const auto right_address = parse_ipv6_address(right.substr(1, right.size() - 2));
        if (left_address || right_address) {
            return left_address && right_address && *left_address == *right_address;
        }
    }
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

namespace {

[[nodiscard]] bool parse_absolute_target(
    http_known_method method, std::string_view target, request_target_view& output) noexcept {
    // RFC 9112 section 3.2.2 defines absolute-form as an absolute-URI, but
    // every caller is an origin server (or builds one of its requests). An
    // origin only serves http(s) resources (RFC 9110 sections 4.2.1-4.2.2),
    // and accepting other schemes would let a front proxy that recognises
    // only http(s) URIs see a different target than the one routed here.
    const auto scheme_end = target.find(':');
    if (scheme_end == std::string_view::npos) {
        return false;
    }
    const auto scheme = target.substr(0, scheme_end);
    if (!http_ascii_equals_ignore_case(scheme, "http") && !http_ascii_equals_ignore_case(scheme, "https")) {
        return false;
    }

    const auto remainder = target.substr(scheme_end + 1);
    const auto query_separator = remainder.find('?');
    const auto hierarchy =
        query_separator == std::string_view::npos ? remainder : remainder.substr(0, query_separator);
    const auto query = query_separator == std::string_view::npos
                           ? std::string_view{}
                           : remainder.substr(query_separator + 1);
    // HTTP(S) URIs have a mandatory "//" authority.
    if (!hierarchy.starts_with("//") || !is_valid_uri_component(query, true, true)) {
        return false;
    }

    const auto authority_and_path = hierarchy.substr(2);
    const auto path_separator = authority_and_path.find('/');
    const auto authority = path_separator == std::string_view::npos
                               ? authority_and_path
                               : authority_and_path.substr(0, path_separator);
    const auto path = path_separator == std::string_view::npos ? std::string_view{}
                                                               : authority_and_path.substr(path_separator);
    // The authority becomes the authoritative Host value, so it must be a
    // non-empty HTTP Host. Userinfo is rejected at this trust boundary (RFC
    // 9110 section 4.2.4) instead of being silently stripped.
    if (authority.empty() || !is_valid_host_header(authority) || !is_valid_uri_component(path, true, false)) {
        return false;
    }

    if (path.empty() && method == http_known_method::options && query_separator == std::string_view::npos) {
        // RFC 9112 section 3.2.4: a proxy forwarding an absolute-form
        // OPTIONS target with an empty path and no query to the final origin
        // must use asterisk-form. Expose that route semantic directly even
        // when this parser itself is the origin-facing recipient.
        output.path_ = "*";
    } else if (path.empty()) {
        // RFC 9110 section 4.2.3; an OPTIONS target with a query remains
        // distinct from server-wide "*".
        output.path_ = "/";
    } else {
        output.path_ = path;
    }
    output.scheme_ = scheme;
    output.query_ = query;
    output.authority_ = authority;
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
        output.form_ = http_request_target_form::authority;
        return true;
    }
    if (target.empty()) {
        return false;
    }
    if (target.front() == '/') {
        std::string_view path;
        std::string_view query;
        if (!split_origin_form_target(target, path, query)) {
            return false;
        }
        output.scheme_ = {};
        output.path_ = path;
        output.query_ = query;
        output.authority_ = {};
        output.form_ = http_request_target_form::origin;
        return true;
    }
    if (!is_valid_request_target_bytes(target)) {
        return false;
    }
    return parse_absolute_target(method, target, output);
}

}  // namespace ruvia::detail
