#include "http_request.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "http_cache.h"
#include "http_client_protocol.h"
#include "http_connection_fields.h"
#include "http_content_coding.h"
#include "http_header.h"
#include "http_response.h"
#include "http_response_server.h"

namespace ruvia {
http_request& http_request::operator=(http_request&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Publish the whole request before an old descriptor's allocator can
    // reenter. The return reference is fixed before retired storage is freed.
    http_request retired(std::move(*this));
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
    return *this;
}

std::optional<std::string_view> http_request::header(std::string_view name) const noexcept {
    const auto kind = detail::classify_http_field(name);
    if (kind != detail::http_field_kind::other) {
        const auto known_slot = detail::http_field_kind_known_slot(kind);
        const auto index = cached_headers_[known_slot];
        if (index == 0 || index > headers_.size()) {
            return std::nullopt;
        }
        return headers_[index - 1].value();
    }

    for (std::size_t i = headers_.size(); i > 0; --i) {
        const auto index = i - 1;
        if (headers_.kind_at(index) == detail::http_field_kind::other && http_ascii_equals_ignore_case(headers_[index].name(), name)) {
            return headers_[index].value();
        }
    }

    return std::nullopt;
}

std::optional<std::string_view> http_request::last_raw_query_value(std::string_view raw_name) const noexcept {
    std::optional<std::string_view> result;
    (void)visit_url_encoded_pairs(query_string_, [&](std::string_view name, std::string_view value) noexcept {
        if (name == raw_name) {
            result = value;
        }
        return true;
    });
    return result;
}

std::optional<std::string_view> http_request::cookie(std::string_view name) const noexcept {
    if (!detail::http_request_access::has_known_header(*this, detail::http_field_kind::cookie)) {
        return std::nullopt;
    }
    // The last matching pair within one field line wins.
    const auto find_in = [name](std::string_view field_value) noexcept {
        std::optional<std::string_view> result;
        http_visit_cookie_pairs(field_value, [name, &result](std::string_view key, std::string_view value) noexcept {
            if (key == name) {
                result = value;
            }
            return true;
        });
        return result;
    };
    const auto last_cookie = detail::http_request_access::known_header(*this, detail::http_field_kind::cookie);
    if (auto value = find_in(last_cookie)) {
        return value;
    }
    if (!detail::http_request_access::has_repeated_known_header(*this, detail::http_field_kind::cookie)) {
        return std::nullopt;
    }
    const auto last_cookie_index = cached_headers_[detail::http_field_kind_known_slot(detail::http_field_kind::cookie)];
    // The one-based cache points to the last Cookie field; only earlier fields can contribute.
    for (std::size_t i = last_cookie_index - 1; i > 0; --i) {
        const auto index = i - 1;
        if (headers_.kind_at(index) != detail::http_field_kind::cookie) {
            continue;
        }
        if (auto value = find_in(headers_[index].value())) {
            return value;
        }
    }
    return std::nullopt;
}

http_response_coding_selection http_response_coding_selection_for(const http_request& request) noexcept {
    http_response_coding_qualities qualities;
    // Every field line contributes: RFC 9110 section 5.3 combines repeated list fields.
    (void)detail::http_request_access::visit_known_headers(request, detail::http_field_kind::accept_encoding, [&qualities](std::string_view value) noexcept {
        qualities.update(value);
        return true;
    });
    return http_response_coding_selection::select(qualities);
}
namespace {

template <typename field_type>
[[nodiscard]] std::optional<http_request_trailer_error> append_trailer_fields(std::vector<http_header>& retained, http_field_section_budget& retained_budget, std::span<const field_type> fields) {
    auto budget = retained_budget;
    for (const auto& field : fields) {
        if (const auto error = detail::charge_http_request_trailer_field(budget, field.name(), field.value())) {
            return error;
        }
    }
    if (fields.empty()) {
        return std::nullopt;
    }
    const auto field_count = retained.size();
    std::vector<http_header> staged;
    staged.reserve(fields.size());
    for (const auto& field : fields) {
        staged.push_back(http_header::copy_with_lowercase_name(field.name(), field.value()));
    }
    retained.reserve(field_count + staged.size());
    for (auto& field : staged) {
        retained.push_back(std::move(field));
    }
    retained_budget = budget;
    return std::nullopt;
}

}  // namespace

std::optional<http_request_trailer_error> http_request_trailers::append(std::string_view name, std::string_view value) {
    const http_header_view field{name, value};
    return append(std::span<const http_header_view>(&field, 1));
}

std::optional<http_request_trailer_error> http_request_trailers::append(std::span<const http_header_view> fields) {
    return append_trailer_fields(fields_, budget_, fields);
}

std::optional<http_request_trailer_error> http_request_trailers::append(std::span<const http_header> fields) {
    return append_trailer_fields(fields_, budget_, fields);
}

std::optional<http_request_trailer_error> http_request_trailers::append_received(std::span<const http_header> fields) {
    auto budget = budget_;
    std::size_t retained_count = 0;
    for (const auto& field : fields) {
        const auto name = field.name();
        const auto value = field.value();
        if (name.empty() || name.front() == ':' || !detail::is_valid_binary_field_name(name) || !is_valid_http_header_value(value)) {
            return http_request_trailer_error::invalid_field;
        }
        if (!budget.add_field(name, value)) {
            return http_request_trailer_error::section_too_large;
        }
        if (!detail::is_forbidden_http_request_trailer_name(name)) {
            ++retained_count;
        }
    }
    if (retained_count == 0) {
        budget_ = budget;
        return std::nullopt;
    }
    std::vector<http_header> staged;
    staged.reserve(retained_count);
    for (const auto& field : fields) {
        if (!detail::is_forbidden_http_request_trailer_name(field.name())) {
            staged.emplace_back(field.name(), field.value());
        }
    }
    fields_.reserve(fields_.size() + staged.size());
    for (auto& field : staged) {
        fields_.push_back(std::move(field));
    }
    budget_ = budget;
    return std::nullopt;
}

std::optional<std::string_view> http_request_trailers::field(std::string_view name) const& noexcept {
    return detail::find_last_http_header<http_header>(fields_, name);
}
namespace {

[[nodiscard]] http_target_origin classify_reg_name(std::string_view host, bool decoded) noexcept {
    if (const auto address = detail::parse_ipv4_address(host)) {
        return http_target_origin::ip_address(http_target_host_kind::ipv4, *address);
    }
    // An absolute domain name names the same host as its relative spelling.
    const bool trailing_dot = host.size() > 1 && host.back() == '.';
    const auto name = trailing_dot ? host.substr(0, host.size() - 1) : host;
    if (!detail::is_valid_dns_domain_name(name) || (trailing_dot && detail::parse_ipv4_address(name).has_value())) {
        return http_target_origin::misdirected();
    }
    return decoded ? http_target_origin::decoded_dns_name(name) : http_target_origin::dns_name(name);
}

[[nodiscard]] http_target_origin certificate_check(std::string_view authority) noexcept {
    // The parsers admitted only valid Host syntax; anything else here is a
    // caller-built request, which fails closed.
    const auto parsed = parse_http_authority(authority);
    if (!parsed.has_value()) {
        return http_target_origin::misdirected();
    }
    const auto host = parsed->host();
    if (host.front() == '[') {
        const auto groups = detail::parse_ipv6_address(host.substr(1, host.size() - 2));
        if (!groups.has_value()) {
            // IPvFuture: no certificate identifies such a host.
            return http_target_origin::misdirected();
        }
        std::array<std::uint8_t, 16> address{};
        for (std::size_t i = 0; i < groups->size(); ++i) {
            address[2 * i] = static_cast<std::uint8_t>((*groups)[i] >> 8);
            address[2 * i + 1] = static_cast<std::uint8_t>((*groups)[i] & 0xFFU);
        }
        return http_target_origin::ip_address(http_target_host_kind::ipv6, address);
    }
    if (host.find('%') == std::string_view::npos) {
        return classify_reg_name(host, false);
    }
    // RFC 3986 §3.2.2 / §6.2.2.2 and RFC 9110 §4.2.3: percent-encoded
    // unreserved octets in reg-name are equivalent to their decoded characters.
    std::array<char, http_target_origin::max_dns_name_size + 1> decoded;
    std::size_t size = 0;
    bool decoded_any = false;
    for (std::size_t i = 0; i < host.size();) {
        if (host[i] == '%') {
            if (const auto byte = http_try_decode_percent_byte(host, i); byte.has_value() && detail::is_unreserved_byte(*byte)) {
                if (size == decoded.size()) {
                    return http_target_origin::misdirected();
                }
                decoded[size++] = static_cast<char>(*byte);
                i += 3;
                decoded_any = true;
                continue;
            }
        }
        if (size == decoded.size()) {
            return http_target_origin::misdirected();
        }
        decoded[size++] = host[i++];
    }
    if (!decoded_any) {
        return classify_reg_name(host, false);
    }
    return classify_reg_name(std::string_view(decoded.data(), size), true);
}

}  // namespace

http_target_origin decide_http_target_origin(const http_request& request, http_target_origin_connection connection) noexcept {
    if (connection.trusted_gateway_) {
        return http_target_origin::accepted();
    }
    const auto form = request.target_form();
    const auto scheme = request.scheme();
    if (form == http_request_target_form::authority || ((form == http_request_target_form::http2 || form == http_request_target_form::http3) && scheme.empty())) {
        // A CONNECT tunnel target names a destination, not an "https" resource.
        return http_target_origin::accepted();
    }
    const bool https = scheme.empty() ? connection.secured_ : http_ascii_equals_ignore_case(scheme, "https");
    if (!https) {
        return http_target_origin::accepted();
    }
    if (!connection.secured_) {
        return http_target_origin::misdirected();
    }
    const auto authority = !request.authority().empty() ? request.authority() : detail::http_request_access::known_header(request, detail::http_field_kind::host);
    if (authority.empty()) {
        return http_target_origin::accepted();
    }
    return certificate_check(authority);
}

namespace detail {

struct http_authority_view_access final {
    [[nodiscard]] static constexpr http_authority_view make(std::string_view host, http_authority_port_kind port_kind, std::uint16_t port = 0) noexcept {
        return http_authority_view(host, port_kind, port);
    }
};

}  // namespace detail

bool is_valid_http_origin_form_target(std::string_view target) noexcept {
    return detail::is_valid_origin_form_target(target);
}

bool is_valid_http_connect_authority(std::string_view authority) noexcept {
    detail::request_target_view target;
    return detail::parse_request_target(http_known_method::connect, authority, target);
}

bool is_valid_http_ipv4_literal(std::string_view value) noexcept {
    return detail::parse_ipv4_address(value).has_value();
}

bool is_valid_http_ipv6_literal(std::string_view value) noexcept {
    return detail::parse_ipv6_address(value).has_value();
}

std::optional<http_authority_view> parse_http_authority(borrowed_text text) noexcept {
    const auto value = text.view();
    if (value.empty()) {
        return std::nullopt;
    }

    std::string_view host;
    std::string_view port_text;
    bool has_port_delimiter = false;
    if (!detail::split_uri_host_port(value, host, port_text, has_port_delimiter)) {
        return std::nullopt;
    }

    if (!detail::is_valid_http_host(host)) {
        return std::nullopt;
    }
    if (!has_port_delimiter) {
        return detail::http_authority_view_access::make(host, http_authority_port_kind::absent);
    }
    if (port_text.empty()) {
        return detail::http_authority_view_access::make(host, http_authority_port_kind::empty);
    }

    const auto port = detail::parse_port_value(port_text);
    if ((port.index() != 0)) {
        return std::nullopt;
    }
    return detail::http_authority_view_access::make(host, http_authority_port_kind::value, std::get<0>(port));
}

std::optional<std::string_view> parse_http_authority_host(borrowed_text value) noexcept {
    if (value.empty()) {
        return value.view();
    }
    const auto authority = parse_http_authority(value);
    if (!authority) {
        return std::nullopt;
    }
    return authority->host();
}

bool http_authorities_equal(borrowed_text left, borrowed_text right, port_number default_port) noexcept {
    return detail::authority_matches_host(left.view(), right.view(), default_port.value());
}

std::size_t estimate_http1_field_section_bytes(const http_response_headers& fields, bool include_terminating_crlf) noexcept {
    return detail::estimate_http1_fields_bytes_impl(fields, include_terminating_crlf);
}

std::size_t estimate_http1_response_head_bytes(http_status_code status, const http_response_headers& headers) noexcept {
    const std::size_t status_line_bytes = 15U + http_reason_phrase(status).size();
    return detail::saturating_add_size(status_line_bytes, estimate_http1_field_section_bytes(headers, true));
}

std::string normalize_http_proxy_request_target(http_known_method method, std::string_view target) {
    if (target.empty() || target.front() == '/' || target == "*" || method == http_known_method::connect) {
        return std::string(target);
    }
    const auto scheme_sep = target.find("://");
    if (scheme_sep == std::string_view::npos || !detail::is_valid_uri_scheme(target.substr(0, scheme_sep))) {
        return std::string(target);
    }
    const auto remainder = target.substr(scheme_sep + 3U);
    const auto query_pos = remainder.find('?');
    const auto authority_and_path = query_pos == std::string_view::npos ? remainder : remainder.substr(0, query_pos);
    const auto query_suffix = query_pos == std::string_view::npos ? std::string_view{} : remainder.substr(query_pos);
    const auto slash_pos = authority_and_path.find('/');
    const auto authority = slash_pos == std::string_view::npos ? authority_and_path : authority_and_path.substr(0, slash_pos);
    const auto path = slash_pos == std::string_view::npos ? std::string_view{} : authority_and_path.substr(slash_pos);
    if (authority.empty()) {
        return std::string(target);
    }
    if (path.empty()) {
        if (query_suffix.empty() && method == http_known_method::options) {
            return std::string("*");
        }
        std::string result;
        result.reserve(1U + query_suffix.size());
        result.push_back('/');
        result.append(query_suffix);
        return result;
    }
    std::string result;
    result.reserve(path.size() + query_suffix.size());
    result.append(path);
    result.append(query_suffix);
    return result;
}

std::string normalize_http_proxy_request_target(std::string_view method, std::string_view target) {
    return normalize_http_proxy_request_target(classify_http_method(method), target);
}

std::string normalize_http_proxy_request_target(const http_request& request) {
    if (request.target_form() == http_request_target_form::authority) {
        return std::string(request.authority());
    }
    const auto path = request.path();
    const auto query = request.query_string();
    if (query.empty()) {
        return std::string(path);
    }
    std::string result;
    result.reserve(path.size() + 1U + query.size());
    result.append(path);
    result.push_back('?');
    result.append(query);
    return result;
}

std::string http_max_forwards_result::format_decremented() const {
    const auto next_val = decremented();
    if (!next_val.has_value()) {
        return {};
    }
    std::array<char, http_max_unsigned_decimal_size> buffer{};
    return std::string(http_format_unsigned(buffer, *next_val));
}

std::optional<std::uint64_t> parse_http_max_forwards_value(std::string_view field_value, std::uint64_t max_saturation) noexcept {
    const auto trimmed = http_trim_ows(field_value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    constexpr auto max_u64 = (std::numeric_limits<std::uint64_t>::max)();
    for (const char ch : trimmed) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        value = value > (max_u64 - digit) / 10U ? max_u64 : value * 10U + digit;
    }
    return (std::min)(value, max_saturation);
}

http_max_forwards_result parse_http_max_forwards(std::string_view field_value, std::uint64_t max_saturation) noexcept {
    const auto parsed = parse_http_max_forwards_value(field_value, max_saturation);
    return parsed.has_value() ? http_max_forwards_result::valid(*parsed) : http_max_forwards_result::invalid();
}

namespace {

template <typename header_span_type>
[[nodiscard]] http_max_forwards_result parse_max_forwards_from_headers(header_span_type headers, std::uint64_t max_saturation) noexcept {
    std::string_view found_value{};
    std::size_t count = 0;
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Max-Forwards")) {
            found_value = field.value();
            ++count;
        }
    }
    if (count == 0) {
        return http_max_forwards_result::absent();
    }
    if (count > 1) {
        return http_max_forwards_result::invalid();
    }
    return parse_http_max_forwards(found_value, max_saturation);
}

}  // namespace

http_max_forwards_result parse_http_max_forwards(std::span<const http_header> headers, std::uint64_t max_saturation) noexcept {
    return parse_max_forwards_from_headers(headers, max_saturation);
}

http_max_forwards_result parse_http_max_forwards(std::span<const http_header_view> headers, std::uint64_t max_saturation) noexcept {
    return parse_max_forwards_from_headers(headers, max_saturation);
}

http_max_forwards_result parse_http_max_forwards(const http_request& request, std::uint64_t max_saturation) noexcept {
    return parse_max_forwards_from_headers(request.headers(), max_saturation);
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

struct normalized_host_unit final {
    unsigned char byte_{0};
    bool encoded_reserved_{false};
};

[[nodiscard]] bool next_normalized_host_unit(std::string_view value, std::size_t& cursor_value, normalized_host_unit& output) noexcept {
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
[[nodiscard]] bool split_origin_form_target(std::string_view target, std::string_view& path, std::string_view& query) noexcept {
    if (target.empty() || target.front() != '/') {
        return false;
    }
    const auto separator = target.find('?');
    path = separator == std::string_view::npos ? target : target.substr(0, separator);
    query = separator == std::string_view::npos ? std::string_view{} : target.substr(separator + 1);
    return is_valid_uri_component(path, true, false) && is_valid_uri_component(query, true, true);
}

}  // namespace

bool is_valid_uri_authority(std::string_view value) noexcept {
    auto host_and_port = value;
    if (const auto delimiter = value.find('@'); delimiter != std::string_view::npos) {
        if (!is_valid_uri_userinfo(value.substr(0, delimiter)) || value.find('@', delimiter + 1) != std::string_view::npos) {
            return false;
        }
        host_and_port = value.substr(delimiter + 1);
    }

    std::string_view host;
    std::string_view port;
    bool has_port = false;
    if (!split_uri_host_port(host_and_port, host, port, has_port)) {
        return false;
    }

    return (host.empty() || is_valid_http_host(host)) && (!has_port || is_valid_uri_port(port));
}

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
        if (left_unit.byte_ != right_unit.byte_ || left_unit.encoded_reserved_ != right_unit.encoded_reserved_) {
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
    if (value.empty() || !is_uri_alpha(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (const auto c : value.substr(1)) {
        const auto byte = static_cast<unsigned char>(c);
        if (!is_uri_alpha(byte) && !is_decimal_digit(c) && byte != '+' && byte != '-' && byte != '.') {
            return false;
        }
    }
    return true;
}

namespace {

[[nodiscard]] bool parse_absolute_target(http_known_method method, std::string_view target, request_target_view& output) noexcept {
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
    const auto hierarchy = query_separator == std::string_view::npos ? remainder : remainder.substr(0, query_separator);
    const auto query = query_separator == std::string_view::npos ? std::string_view{} : remainder.substr(query_separator + 1);
    // HTTP(S) URIs have a mandatory "//" authority.
    if (!hierarchy.starts_with("//") || !is_valid_uri_component(query, true, true)) {
        return false;
    }

    const auto authority_and_path = hierarchy.substr(2);
    const auto path_separator = authority_and_path.find('/');
    const auto authority = path_separator == std::string_view::npos ? authority_and_path : authority_and_path.substr(0, path_separator);
    const auto path = path_separator == std::string_view::npos ? std::string_view{} : authority_and_path.substr(path_separator);
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

bool authority_matches_host(std::string_view authority, std::string_view host, std::uint16_t default_port) noexcept {
    const auto authority_parts = parse_http_authority(authority);
    const auto host_parts = parse_http_authority(host);
    if (!authority_parts || !host_parts || !http_uri_host_equals(authority_parts->host(), host_parts->host())) {
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

bool parse_request_target(http_known_method method, std::string_view target, request_target_view& output) noexcept {
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

std::uint16_t http_uri_scheme_default_port(std::string_view scheme) noexcept {
    if (http_ascii_equals_ignore_case(scheme, "https")) {
        return http_scheme_default_port(http_scheme::https);
    }
    return http_ascii_equals_ignore_case(scheme, "http") ? http_scheme_default_port(http_scheme::http) : 0;
}
}  // namespace ruvia::detail

namespace ruvia {
namespace {

using detail::is_decimal_digit;
using detail::is_uri_lower_alpha;
using detail::parse_ipv4_address;
using detail::parse_port_value;

[[nodiscard]] bool is_lower_alpha_numeric(char value) noexcept {
    return is_uri_lower_alpha(static_cast<unsigned char>(value)) || is_decimal_digit(value);
}

[[nodiscard]] bool is_valid_serialized_origin_scheme(std::string_view scheme) noexcept {
    if (scheme.empty() || !is_uri_lower_alpha(static_cast<unsigned char>(scheme.front()))) {
        return false;
    }
    for (const auto value : scheme.substr(1)) {
        if (!is_lower_alpha_numeric(value) && value != '+' && value != '-' && value != '.') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_serialized_origin_ipv4_number(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (std::ranges::all_of(value, is_decimal_digit)) {
        return true;
    }
    if (value.size() >= 2 && value.front() == '0' && (value[1] == 'x' || value[1] == 'X')) {
        return std::ranges::all_of(value.substr(2), [](char digit) noexcept {
            return http_decode_hex_nibble(digit) >= 0;
        });
    }
    if (value.size() >= 2 && value.front() == '0') {
        return std::ranges::all_of(value.substr(1), [](char digit) noexcept {
            return digit >= '0' && digit <= '7';
        });
    }
    return false;
}

[[nodiscard]] bool serialized_origin_domain_ends_in_number(std::string_view domain) noexcept {
    if (!domain.empty() && domain.back() == '.') {
        domain.remove_suffix(1);
    }
    const auto separator = domain.rfind('.');
    const auto last_label = separator == std::string_view::npos ? domain : domain.substr(separator + 1);
    return is_serialized_origin_ipv4_number(last_label);
}

[[nodiscard]] bool is_serialized_origin_domain_byte(unsigned char byte) noexcept {
    if (byte >= 0x80 || byte <= 0x20 || byte == 0x7f || (byte >= 'A' && byte <= 'Z')) {
        return false;
    }
    switch (byte) {
        case '#':
        case '%':
        case '/':
        case ':':
        case '<':
        case '>':
        case '?':
        case '@':
        case '[':
        case '\\':
        case ']':
        case '^':
        case '|':
            return false;
        default:
            return true;
    }
}

[[nodiscard]] bool is_valid_serialized_origin_domain(std::string_view domain) noexcept {
    if (domain.empty()) {
        return false;
    }
    if (serialized_origin_domain_ends_in_number(domain)) {
        return false;
    }
    return std::ranges::all_of(domain, [](char byte) noexcept {
        return is_serialized_origin_domain_byte(static_cast<unsigned char>(byte));
    });
}

[[nodiscard]] std::size_t find_serialized_origin_ipv6_compression_index(const std::array<std::uint16_t, 8>& pieces) noexcept {
    constexpr std::size_t no_compression = 8;
    std::size_t longest_index = no_compression;
    std::size_t longest_size = 1;
    std::size_t found_index = no_compression;
    std::size_t found_size = 0;

    for (std::size_t piece_index = 0; piece_index < pieces.size(); ++piece_index) {
        if (pieces[piece_index] != 0) {
            if (found_size > longest_size) {
                longest_index = found_index;
                longest_size = found_size;
            }
            found_index = no_compression;
            found_size = 0;
            continue;
        }
        if (found_index == no_compression) {
            found_index = piece_index;
        }
        ++found_size;
    }
    if (found_size > longest_size) {
        return found_index;
    }
    return longest_index;
}

[[nodiscard]] bool append_serialized_origin_ipv6_hex(std::array<char, 39>& output, std::size_t& size, std::uint16_t value) noexcept {
    bool emitted = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((value >> shift) & 0xFU);
        if (nibble == 0 && !emitted && shift != 0) {
            continue;
        }
        if (size == output.size()) {
            return false;
        }
        output[size++] = http_lower_hex_digit(static_cast<int>(nibble));
        emitted = true;
    }
    return true;
}

[[nodiscard]] bool append_serialized_origin_ipv6_byte(std::array<char, 39>& output, std::size_t& size, char byte) noexcept {
    if (size == output.size()) {
        return false;
    }
    output[size++] = byte;
    return true;
}

[[nodiscard]] bool is_canonical_serialized_origin_ipv6(std::string_view literal, const std::array<std::uint16_t, 8>& pieces) noexcept {
    std::array<char, 39> serialized{};
    std::size_t size = 0;
    const auto compression_index = find_serialized_origin_ipv6_compression_index(pieces);
    bool ignore_zero = false;

    for (std::size_t piece_index = 0; piece_index < pieces.size(); ++piece_index) {
        if (ignore_zero && pieces[piece_index] == 0) {
            continue;
        }
        if (ignore_zero) {
            ignore_zero = false;
        }
        if (compression_index == piece_index) {
            if (piece_index == 0) {
                if (!append_serialized_origin_ipv6_byte(serialized, size, ':')) {
                    return false;
                }
            }
            if (!append_serialized_origin_ipv6_byte(serialized, size, ':')) {
                return false;
            }
            ignore_zero = true;
            continue;
        }
        if (!append_serialized_origin_ipv6_hex(serialized, size, pieces[piece_index])) {
            return false;
        }
        if (piece_index != pieces.size() - 1 && !append_serialized_origin_ipv6_byte(serialized, size, ':')) {
            return false;
        }
    }

    return literal.size() == size && std::equal(serialized.begin(), serialized.begin() + static_cast<std::ptrdiff_t>(size), literal.begin());
}

// The canonical re-serialization (lowercase, no leading zeros, longest-run
// "::", no dotted IPv4) is the gate; the RFC 3986 parser only supplies pieces.
[[nodiscard]] bool is_valid_serialized_origin_ipv6(std::string_view literal) noexcept {
    const auto pieces = detail::parse_ipv6_address(literal);
    return pieces.has_value() && is_canonical_serialized_origin_ipv6(literal, *pieces);
}

[[nodiscard]] std::variant<std::uint16_t, std::errc> parse_serialized_origin_port(std::string_view value) noexcept {
    // A serialized URL port is the shortest decimal form of the URL record's
    // 16-bit port. Merely accepting five digits admits values such as 99999,
    // while accepting leading zeroes admits spellings no serializer can emit.
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return std::errc::invalid_argument;
    }
    return parse_port_value(value);
}

[[nodiscard]] std::optional<std::uint16_t> serialized_origin_default_port(std::string_view scheme) noexcept {
    if (scheme == "ftp") {
        return 21;
    }
    if (scheme == "http" || scheme == "ws") {
        return 80;
    }
    if (scheme == "https" || scheme == "wss") {
        return 443;
    }
    return std::nullopt;
}

}  // namespace

bool is_valid_http_serialized_origin(std::string_view value) noexcept {
    const auto scheme_end = value.find("://");
    const auto scheme = value.substr(0, scheme_end);
    if (scheme_end == std::string_view::npos || !is_valid_serialized_origin_scheme(scheme)) {
        return false;
    }

    const auto authority = value.substr(scheme_end + 3);
    if (authority.empty()) {
        return false;
    }

    std::string_view host;
    std::string_view port;
    bool has_port = false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close == 1) {
            return false;
        }
        host = authority.substr(1, close - 1);
        const auto remainder = authority.substr(close + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':') {
                return false;
            }
            has_port = true;
            port = remainder.substr(1);
        }
        if (!is_valid_serialized_origin_ipv6(host)) {
            return false;
        }
    } else {
        const auto port_separator = authority.find(':');
        if (port_separator == std::string_view::npos) {
            host = authority;
        } else {
            if (authority.find(':', port_separator + 1) != std::string_view::npos) {
                return false;
            }
            host = authority.substr(0, port_separator);
            has_port = true;
            port = authority.substr(port_separator + 1);
        }
        if (!parse_ipv4_address(host) && !is_valid_serialized_origin_domain(host)) {
            return false;
        }
    }
    if (!has_port) {
        return true;
    }
    const auto port_value = parse_serialized_origin_port(port);
    if ((port_value.index() != 0)) {
        return false;
    }
    const auto default_port = serialized_origin_default_port(scheme);
    return !default_port.has_value() || std::get<0>(port_value) != *default_port;
}

namespace {

constexpr auto npos = std::string_view::npos;

// Positions whose literal legality depends on the reference's own structure.
struct uri_reference_layout final {
    // The only '[' and ']' that delimit the authority's IP literal.
    std::size_t ip_literal_open_{npos};
    std::size_t ip_literal_close_{npos};
    // Authority bytes [authority_begin_, userinfo_end_) are userinfo, which
    // cannot carry '@' literally (RFC 3986 section 3.2.1); userinfo_end_ is
    // the delimiting '@'.
    std::size_t authority_begin_{npos};
    std::size_t userinfo_end_{npos};
    // Bytes before this index form the colon-free first segment of a
    // relative-path reference; zero when the reference has a scheme or an
    // authority, or starts with a path, query, or fragment delimiter.
    std::size_t colon_free_end_{0};
};

[[nodiscard]] uri_reference_layout find_uri_reference_layout(std::string_view reference) noexcept {
    uri_reference_layout layout;
    const auto first_delimiter = reference.find_first_of("/?#");
    const auto colon = reference.find(':');
    std::size_t authority_begin = npos;
    if (colon < first_delimiter && detail::is_valid_uri_scheme(reference.substr(0, colon))) {
        if (reference.substr(colon + 1).starts_with("//")) {
            authority_begin = colon + 3;
        }
    } else {
        layout.colon_free_end_ = first_delimiter == npos ? reference.size() : first_delimiter;
        if (reference.starts_with("//")) {
            authority_begin = 2;
        }
    }
    if (authority_begin == npos) {
        return layout;
    }

    const auto authority_end_index = reference.find_first_of("/?#", authority_begin);
    const auto authority_end = authority_end_index == npos ? reference.size() : authority_end_index;
    const auto authority = reference.substr(authority_begin, authority_end - authority_begin);
    const auto userinfo_end = authority.rfind('@');
    if (userinfo_end != npos) {
        layout.authority_begin_ = authority_begin;
        layout.userinfo_end_ = authority_begin + userinfo_end;
    }
    const auto host_begin = authority_begin + (userinfo_end == npos ? 0 : userinfo_end + 1);
    if (host_begin >= authority_end || reference[host_begin] != '[') {
        return layout;
    }
    const auto close = reference.find(']', host_begin + 1);
    if (close < authority_end) {
        layout.ip_literal_open_ = host_begin;
        layout.ip_literal_close_ = close;
    }
    return layout;
}

[[nodiscard]] bool uri_reference_keeps_literal(unsigned char byte, std::size_t index, const uri_reference_layout& layout, bool& fragment_seen) noexcept {
    switch (byte) {
        case '[':
        case ']':
            return index == layout.ip_literal_open_ || index == layout.ip_literal_close_;
        case ':':
            return index >= layout.colon_free_end_;
        case '#': {
            const bool keeps_separator = !fragment_seen;
            fragment_seen = true;
            return keeps_separator;
        }
        case '@':
            return index < layout.authority_begin_ || index >= layout.userinfo_end_;
        case '/':
        case '?':
            return true;
        default:
            return detail::is_uri_pchar(byte);
    }
}

}  // namespace

std::string_view percent_encode_uri_reference(borrowed_text text, std::string& storage) {
    const auto reference = text.view();
    const auto layout = find_uri_reference_layout(reference);
    bool fragment_seen = false;
    const bool initially_empty = storage.empty();
    std::string staged;
    std::string* encoded = nullptr;
    try {
        for (std::size_t i = 0; i < reference.size(); ++i) {
            const auto byte = static_cast<unsigned char>(reference[i]);
            // A lone or malformed '%' is not an escape and is encoded like any
            // other octet the reference cannot carry literally.
            const bool escape = byte == '%' && http_try_decode_percent_byte(reference, i).has_value();
            if (escape || (byte != '%' && uri_reference_keeps_literal(byte, i, layout, fragment_seen))) {
                const std::size_t width = escape ? 3 : 1;
                if (encoded != nullptr) {
                    if (width > encoded->max_size() - encoded->size()) {
                        throw std::length_error("URI reference exceeds string capacity");
                    }
                    detail::reserve_geometric(*encoded, encoded->size() + width);
                    encoded->append(reference.substr(i, width));
                }
                i += width - 1;
                continue;
            }
            if (encoded == nullptr) {
                // Nonempty storage can be overwritten only when every possible
                // triplet fits without another allocation. Empty storage rolls back
                // by clearing; aliased input always needs independent storage.
                const bool can_reuse = (initially_empty || reference.size() <= storage.capacity() / 3) && !detail::http_text_aliases_string(reference, storage);
                encoded = can_reuse ? &storage : &staged;
                if (reference.size() > encoded->max_size() - 2) {
                    throw std::length_error("URI reference exceeds string capacity");
                }
                detail::reserve_geometric(*encoded, reference.size() + 2);
                encoded->clear();
                encoded->append(reference.substr(0, i));
            }
            if (encoded->size() > encoded->max_size() - 3) {
                throw std::length_error("URI reference exceeds string capacity");
            }
            detail::reserve_geometric(*encoded, encoded->size() + 3);
            http_append_percent_byte(*encoded, byte);
        }
    } catch (...) {
        if (initially_empty && encoded == &storage) {
            storage.clear();
        }
        throw;
    }
    if (encoded == nullptr) {
        return reference;
    }
    if (encoded == &staged) {
        storage = std::move(staged);
    }
    return storage;
}

std::optional<std::string_view> decode_url_component(borrowed_text text, std::string& storage, url_decode_mode mode) {
    const auto input = text.view();
    std::string staged;
    std::string* target = nullptr;
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        std::size_t advance = 0;
        bool transformed = false;
        if (mode == url_decode_mode::form && c == '+') {
            c = ' ';
            transformed = true;
        } else if (c == '%') {
            if (const auto byte = http_try_decode_percent_byte(input, i)) {
                c = static_cast<char>(*byte);
                advance = 2;
                transformed = true;
            } else if (mode != url_decode_mode::form) {
                if (target == &storage) {
                    storage.clear();
                }
                return std::nullopt;
            }
        }
        if (!transformed) {
            if (target != nullptr) {
                target->push_back(c);
            }
            continue;
        }
        if (target == nullptr) {
            const bool can_write_in_place = (mode == url_decode_mode::form || storage.empty()) && !detail::http_text_aliases_string(input, storage);
            target = can_write_in_place ? &storage : &staged;
            target->reserve(input.size() - advance);
            target->clear();
            if (i != 0) {
                target->append(input.data(), i);
            }
        }
        target->push_back(c);
        i += advance;
    }
    if (target == nullptr) {
        return input;
    }
    if (target == &staged) {
        storage = std::move(staged);
    }
    return std::string_view(storage);
}

}  // namespace ruvia

namespace ruvia::detail {

namespace {

[[nodiscard]] constexpr bool is_uri_sub_delimiter(unsigned char byte) noexcept {
    switch (byte) {
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
            return true;
        default:
            return false;
    }
}

inline constexpr std::array<bool, 256> reg_name_char_table = [] {
    std::array<bool, 256> table_value{};
    for (std::size_t value = 0; value < table_value.size(); ++value) {
        const auto byte = static_cast<unsigned char>(value);
        table_value[value] = is_unreserved_byte(byte) || is_uri_sub_delimiter(byte);
    }
    return table_value;
}();

[[nodiscard]] constexpr bool is_uri_userinfo_literal(unsigned char byte) noexcept {
    return reg_name_char_table[byte] || byte == ':';
}

[[nodiscard]] bool parse_ipv6_hex_group(std::string_view literal, std::size_t& offset, std::uint16_t& group) noexcept {
    group = 0;
    std::size_t digits = 0;
    while (offset < literal.size() && digits < 4) {
        const auto nibble = http_decode_hex_nibble(literal[offset]);
        if (nibble < 0) {
            break;
        }
        group = static_cast<std::uint16_t>((group << 4) | nibble);
        ++offset;
        ++digits;
    }
    return digits != 0;
}

template <typename is_allowed_type>
[[nodiscard]] bool is_valid_percent_encoded(std::string_view value, is_allowed_type is_allowed) noexcept {
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte == '%') {
            if (!http_try_decode_percent_byte(value, index)) {
                return false;
            }
            index += 2;
        } else if (!is_allowed(byte)) {
            return false;
        }
    }
    return true;
}

}  // namespace

[[nodiscard]] bool is_valid_uri_userinfo(std::string_view value) noexcept {
    return is_valid_percent_encoded(value, [](unsigned char byte) noexcept {
        return is_uri_userinfo_literal(byte);
    });
}

bool is_valid_uri_port(std::string_view value) noexcept {
    return std::ranges::all_of(value, is_decimal_digit);
}

bool split_uri_host_port(std::string_view value, std::string_view& host, std::string_view& port, bool& has_port) noexcept {
    port = {};
    has_port = false;
    if (!value.empty() && value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        host = value.substr(0, close + 1);
        const auto remainder = value.substr(close + 1);
        if (remainder.empty()) {
            return true;
        }
        if (remainder.front() != ':') {
            return false;
        }
        has_port = true;
        port = remainder.substr(1);
        return true;
    }
    const auto delimiter = value.find(':');
    if (delimiter == std::string_view::npos) {
        host = value;
        return true;
    }
    host = value.substr(0, delimiter);
    has_port = true;
    port = value.substr(delimiter + 1);
    return port.find(':') == std::string_view::npos;
}

[[nodiscard]] bool is_uri_pchar(unsigned char byte) noexcept {
    return is_uri_userinfo_literal(byte) || byte == '@';
}

std::variant<std::uint16_t, std::errc> parse_port_value(std::string_view value) noexcept {
    if (value.empty()) {
        return std::errc::invalid_argument;
    }

    std::uint16_t parsed_value = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed_value);
    if (ec != std::errc{}) {
        return ec;
    }
    if (ptr != end) {
        return std::errc::invalid_argument;
    }
    return parsed_value;
}

[[nodiscard]] bool is_valid_uri_component(std::string_view value, bool allow_slash, bool allow_question) noexcept {
    return is_valid_percent_encoded(value, [allow_slash, allow_question](unsigned char byte) noexcept {
        return is_uri_pchar(byte) || (allow_slash && byte == '/') || (allow_question && byte == '?');
    });
}

std::optional<std::array<std::uint8_t, 4>> parse_ipv4_address(std::string_view value) noexcept {
    std::array<std::uint8_t, 4> octets;
    std::size_t offset = 0;
    for (std::size_t part = 0; part < octets.size(); ++part) {
        if (offset >= value.size() || !is_decimal_digit(value[offset])) {
            return std::nullopt;
        }
        const auto part_begin = offset;
        unsigned int octet = 0;
        std::size_t digits = 0;
        while (offset < value.size() && is_decimal_digit(value[offset])) {
            octet = octet * 10 + static_cast<unsigned int>(value[offset] - '0');
            ++offset;
            ++digits;
            if (digits > 3 || octet > 255) {
                return std::nullopt;
            }
        }
        if (digits > 1 && value[part_begin] == '0') {
            return std::nullopt;
        }
        octets[part] = static_cast<std::uint8_t>(octet);
        if (part + 1 == octets.size()) {
            return offset == value.size() ? std::optional(octets) : std::nullopt;
        }
        if (offset >= value.size() || value[offset] != '.') {
            return std::nullopt;
        }
        ++offset;
    }
    return std::nullopt;
}

std::optional<std::array<std::uint16_t, 8>> parse_ipv6_address(std::string_view literal) noexcept {
    if (literal.empty()) {
        return std::nullopt;
    }

    std::array<std::uint16_t, 8> groups{};
    std::size_t offset = 0;
    std::size_t size = 0;
    auto compression = std::string_view::npos;
    if (literal.starts_with("::")) {
        compression = 0;
        offset = 2;
    }

    while (offset < literal.size()) {
        if (size == groups.size()) {
            return std::nullopt;
        }
        const auto next_dot = literal.find('.', offset);
        const auto next_colon = literal.find(':', offset);
        if (is_decimal_digit(literal[offset]) && next_dot != std::string_view::npos && (next_colon == std::string_view::npos || next_dot < next_colon)) {
            const auto tail = parse_ipv4_address(literal.substr(offset));
            if (!tail || size > groups.size() - 2) {
                return std::nullopt;
            }
            groups[size++] = static_cast<std::uint16_t>((*tail)[0] << 8 | (*tail)[1]);
            groups[size++] = static_cast<std::uint16_t>((*tail)[2] << 8 | (*tail)[3]);
            break;
        }
        if (!parse_ipv6_hex_group(literal, offset, groups[size])) {
            return std::nullopt;
        }
        ++size;
        if (offset == literal.size()) {
            break;
        }
        if (literal[offset] != ':') {
            return std::nullopt;
        }
        if (offset + 1 < literal.size() && literal[offset + 1] == ':') {
            if (compression != std::string_view::npos) {
                return std::nullopt;
            }
            compression = size;
            offset += 2;
        } else {
            ++offset;
            if (offset == literal.size()) {
                return std::nullopt;
            }
        }
    }

    if (compression == std::string_view::npos) {
        return size == groups.size() ? std::optional(groups) : std::nullopt;
    }
    if (size >= groups.size() || compression > size) {
        return std::nullopt;
    }
    // Expand the omitted zero groups without changing the suffix's order.
    std::move_backward(groups.begin() + compression, groups.begin() + size, groups.end());
    std::fill_n(groups.begin() + compression, groups.size() - size, std::uint16_t{0});
    return groups;
}

[[nodiscard]] bool is_valid_ipv_future(std::string_view literal) noexcept {
    if (literal.size() < 4 || (literal.front() != 'v' && literal.front() != 'V')) {
        return false;
    }

    std::size_t cursor_value = 1;
    const auto version_begin = cursor_value;
    while (cursor_value < literal.size() && http_decode_hex_nibble(literal[cursor_value]) >= 0) {
        ++cursor_value;
    }
    if (cursor_value == version_begin || cursor_value >= literal.size() || literal[cursor_value] != '.') {
        return false;
    }
    ++cursor_value;
    if (cursor_value == literal.size()) {
        return false;
    }

    literal.remove_prefix(cursor_value);
    for (const unsigned char byte : literal) {
        if (!is_uri_userinfo_literal(byte)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_valid_reg_name(std::string_view value) noexcept {
    return !value.empty() && is_valid_percent_encoded(value, [](unsigned char byte) noexcept {
        return reg_name_char_table[byte];
    });
}

[[nodiscard]] bool is_valid_dns_domain_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 253) {
        return false;
    }
    std::size_t label_length = 0;
    bool label_ends_with_alnum = false;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        const bool alnum = is_uri_alpha(byte) || is_decimal_digit(c);
        if (c == '.') {
            if (label_length == 0 || !label_ends_with_alnum) {
                return false;
            }
            label_length = 0;
            label_ends_with_alnum = false;
            continue;
        }
        if ((!alnum && c != '-') || label_length == 63 || (label_length == 0 && !alnum)) {
            return false;
        }
        ++label_length;
        label_ends_with_alnum = alnum;
    }
    return label_length != 0 && label_ends_with_alnum;
}

}  // namespace ruvia::detail
