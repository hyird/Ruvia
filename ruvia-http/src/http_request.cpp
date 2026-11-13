#include "ruvia/http/http_request.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
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
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/http_client.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/websocket_handshake.h"

#include "http_connection_fields.h"

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
        if (headers_.kind_at(index) == detail::http_field_kind::other &&
            http_ascii_equals_ignore_case(headers_[index].name(), name)) {
            return headers_[index].value();
        }
    }

    return std::nullopt;
}

std::optional<std::string_view> http_request::last_raw_query_value(
    std::string_view raw_name) const noexcept {
    std::optional<std::string_view> result;
    (void)visit_url_encoded_pairs(
        query_string_, [&](std::string_view name, std::string_view value) noexcept {
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
    const auto last_cookie =
        detail::http_request_access::known_header(*this, detail::http_field_kind::cookie);
    if (auto value = find_in(last_cookie)) {
        return value;
    }
    if (!detail::http_request_access::has_repeated_known_header(
            *this, detail::http_field_kind::cookie)) {
        return std::nullopt;
    }
    const auto last_cookie_index =
        cached_headers_[detail::http_field_kind_known_slot(detail::http_field_kind::cookie)];
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
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::accept_encoding, [&qualities](std::string_view value) noexcept {
            qualities.update(value);
            return true;
        });
    return http_response_coding_selection::select(qualities);
}
}  // namespace ruvia

namespace ruvia {
namespace {

template <typename field_type>
[[nodiscard]] std::optional<http_request_trailer_error> append_trailer_fields(
    std::vector<http_header>& retained, http_field_section_budget& retained_budget,
    std::span<const field_type> fields) {
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
        if (name.empty() || name.front() == ':' || !detail::is_valid_binary_field_name(name) ||
            !is_valid_http_header_value(value)) {
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
}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] http_target_origin classify_reg_name(std::string_view host, bool decoded) noexcept {
    if (const auto address = detail::parse_ipv4_address(host)) {
        return http_target_origin::ip_address(http_target_host_kind::ipv4, *address);
    }
    // An absolute domain name names the same host as its relative spelling.
    const bool trailing_dot = host.size() > 1 && host.back() == '.';
    const auto name = trailing_dot ? host.substr(0, host.size() - 1) : host;
    if (!detail::is_valid_dns_domain_name(name) ||
        (trailing_dot && detail::parse_ipv4_address(name).has_value())) {
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
            if (const auto byte = http_try_decode_percent_byte(host, i);
                byte.has_value() && detail::is_unreserved_byte(*byte)) {
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

http_target_origin decide_http_target_origin(
    const http_request& request, http_target_origin_connection connection) noexcept {
    if (connection.trusted_gateway_) {
        return http_target_origin::accepted();
    }
    const auto form = request.target_form();
    const auto scheme = request.scheme();
    if (form == http_request_target_form::authority ||
        ((form == http_request_target_form::http2 || form == http_request_target_form::http3) &&
            scheme.empty())) {
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
    const auto authority = !request.authority().empty()
                               ? request.authority()
                               : detail::http_request_access::known_header(request, detail::http_field_kind::host);
    if (authority.empty()) {
        return http_target_origin::accepted();
    }
    return certificate_check(authority);
}

}  // namespace ruvia

namespace ruvia {
namespace detail {

struct http_authority_view_access final {
    [[nodiscard]] static constexpr http_authority_view make(
        std::string_view host, http_authority_port_kind port_kind, std::uint16_t port = 0) noexcept {
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

std::size_t estimate_http1_field_section_bytes(
    const http_response_headers& fields, bool include_terminating_crlf) noexcept {
    return detail::estimate_http1_fields_bytes_impl(fields, include_terminating_crlf);
}

std::size_t estimate_http1_response_head_bytes(
    http_status_code status, const http_response_headers& headers) noexcept {
    const std::size_t status_line_bytes = 15U + http_reason_phrase(status).size();
    return detail::saturating_add_size(
        status_line_bytes, estimate_http1_field_section_bytes(headers, true));
}

std::string normalize_http_proxy_request_target(
    http_known_method method, std::string_view target) {
    if (target.empty() || target.front() == '/' || target == "*" ||
        method == http_known_method::connect) {
        return std::string(target);
    }
    const auto scheme_sep = target.find("://");
    if (scheme_sep == std::string_view::npos ||
        !detail::is_valid_uri_scheme(target.substr(0, scheme_sep))) {
        return std::string(target);
    }
    const auto remainder = target.substr(scheme_sep + 3U);
    const auto query_pos = remainder.find('?');
    const auto authority_and_path =
        query_pos == std::string_view::npos ? remainder : remainder.substr(0, query_pos);
    const auto query_suffix =
        query_pos == std::string_view::npos ? std::string_view{} : remainder.substr(query_pos);
    const auto slash_pos = authority_and_path.find('/');
    const auto authority = slash_pos == std::string_view::npos
                               ? authority_and_path
                               : authority_and_path.substr(0, slash_pos);
    const auto path = slash_pos == std::string_view::npos
                          ? std::string_view{}
                          : authority_and_path.substr(slash_pos);
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

std::string normalize_http_proxy_request_target(
    std::string_view method, std::string_view target) {
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

std::optional<std::uint64_t> parse_http_max_forwards_value(
    std::string_view field_value, std::uint64_t max_saturation) noexcept {
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

http_max_forwards_result parse_http_max_forwards(
    std::string_view field_value, std::uint64_t max_saturation) noexcept {
    const auto parsed = parse_http_max_forwards_value(field_value, max_saturation);
    return parsed.has_value() ? http_max_forwards_result::valid(*parsed)
                              : http_max_forwards_result::invalid();
}

namespace {

template <typename header_span_type>
[[nodiscard]] http_max_forwards_result parse_max_forwards_from_headers(
    header_span_type headers, std::uint64_t max_saturation) noexcept {
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

http_max_forwards_result parse_http_max_forwards(
    std::span<const http_header> headers, std::uint64_t max_saturation) noexcept {
    return parse_max_forwards_from_headers(headers, max_saturation);
}

http_max_forwards_result parse_http_max_forwards(
    std::span<const http_header_view> headers, std::uint64_t max_saturation) noexcept {
    return parse_max_forwards_from_headers(headers, max_saturation);
}

http_max_forwards_result parse_http_max_forwards(
    const http_request& request, std::uint64_t max_saturation) noexcept {
    return parse_max_forwards_from_headers(request.headers(), max_saturation);
}
}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] constexpr bool http_origin_uses_default_port(const http_origin_view& origin) noexcept {
    return origin.port() == detail::http_scheme_default_port(origin.scheme());
}

void validate_origin_host(std::string_view host) {
    if (host.empty()) {
        throw std::invalid_argument("HTTP origin host must not be empty");
    }
    if (!detail::is_valid_http_host(host)) {
        throw std::invalid_argument("HTTP origin host is invalid");
    }
}

std::uint16_t resolved_origin_port(http_scheme scheme, const http_origin_options& options) noexcept {
    if (options.port_.has_value()) {
        return options.port_->value();
    }
    return detail::http_scheme_default_port(scheme);
}

void remove_http_client_last_path_segment(std::string& path) noexcept {
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) {
        path.clear();
        return;
    }
    path.erase(slash);
}

// `path` is always absolute: every caller builds it from a '/'-prefixed
// reference path or base directory, so only the '/'-led dot-segment cases of
// RFC 3986 section 5.2.4 can occur.
void normalize_http_client_absolute_path(
    std::string_view path, std::size_t extra_reserve, std::string& normalized) {
    normalized.clear();
    normalized.reserve((path.empty() ? 1U : path.size()) + extra_reserve);
    if (!path.empty() && path.find("/.") == std::string_view::npos) {
        normalized.append(path.data(), path.size());
        return;
    }
    auto remaining = path;
    while (!remaining.empty()) {
        // RFC 3986 section 5.2.4 deliberately moves one path segment at a
        // time. Empty segments are significant: reducing the path to a stack
        // of non-dot segments would turn "/a//." into "/a/" instead of the
        // required "/a//".
        if (remaining.starts_with("/./")) {
            remaining.remove_prefix(2);
        } else if (remaining == "/.") {
            remaining = "/";
        } else if (remaining.starts_with("/../")) {
            remaining.remove_prefix(3);
            remove_http_client_last_path_segment(normalized);
        } else if (remaining == "/..") {
            remaining = "/";
            remove_http_client_last_path_segment(normalized);
        } else {
            const auto next_slash = remaining.find('/', 1);
            const auto segment_bytes =
                next_slash == std::string_view::npos ? remaining.size() : next_slash;
            normalized.append(remaining.substr(0, segment_bytes));
            remaining.remove_prefix(segment_bytes);
        }
    }

    if (normalized.empty()) {
        normalized.push_back('/');
    }
}

// Merges the reference's path/query with the current origin-form target per
// RFC 3986 section 5.2 and validates the resolved origin-form target. Returns
// false when the location's path or the merged product is not a valid target.
// `reference` is the URI-reference with any scheme/authority prefix and the
// fragment already removed.
[[nodiscard]] bool resolve_http_client_redirect_path_and_query(bool has_authority,
    std::string_view reference, std::string_view current_target, std::string& resolved) {
    const auto query_at = reference.find('?');
    const bool has_reference_query = query_at != std::string_view::npos;
    const auto reference_path = has_reference_query ? reference.substr(0, query_at) : reference;
    const auto reference_query =
        has_reference_query ? reference.substr(query_at + 1) : std::string_view{};
    // Dot-segment removal must not hide malformed bytes in discarded segments.
    if (!detail::is_valid_uri_component(reference_path, true, false) ||
        !detail::is_valid_uri_component(reference_query, true, true)) {
        return false;
    }

    static constexpr std::size_t inline_path_capacity = 256;
    std::array<char, inline_path_capacity> inline_merged_path{};
    std::string merged_path_storage;
    std::string_view merged_path;
    std::string_view selected_query;
    bool has_selected_query = has_reference_query;

    if (has_authority) {
        if (!reference_path.empty() && reference_path.front() != '/') {
            return false;
        }
        merged_path = reference_path.empty() ? std::string_view("/") : reference_path;
        selected_query = reference_query;
    } else {
        const auto base_query_at = current_target.find('?');
        const bool has_base_query = base_query_at != std::string_view::npos;
        const auto base_path = has_base_query ? current_target.substr(0, base_query_at) : current_target;
        const auto base_query =
            has_base_query ? current_target.substr(base_query_at + 1) : std::string_view{};

        if (reference_path.empty()) {
            merged_path = base_path;
            if (has_reference_query) {
                selected_query = reference_query;
            } else {
                has_selected_query = has_base_query;
                selected_query = base_query;
            }
        } else if (reference_path.front() == '/') {
            merged_path = reference_path;
            selected_query = reference_query;
        } else {
            const auto prefix_size = base_path.rfind('/') + 1U;
            const auto total_path_size = prefix_size + reference_path.size();
            if (total_path_size <= inline_merged_path.size()) {
                std::memcpy(inline_merged_path.data(), base_path.data(), prefix_size);
                std::memcpy(inline_merged_path.data() + prefix_size, reference_path.data(),
                    reference_path.size());
                merged_path = std::string_view(inline_merged_path.data(), total_path_size);
            } else {
                merged_path_storage.reserve(total_path_size);
                merged_path_storage.append(base_path.data(), prefix_size);
                merged_path_storage.append(reference_path.data(), reference_path.size());
                merged_path = merged_path_storage;
            }
            selected_query = reference_query;
        }
    }

    const std::size_t query_extra = has_selected_query ? 1U + selected_query.size() : 0U;
    if (!has_authority && reference_path.empty()) {
        // RFC 3986 section 5.2.2 copies the base path for an empty reference path.
        resolved.clear();
        resolved.reserve(merged_path.size() + query_extra);
        resolved.append(merged_path.data(), merged_path.size());
    } else {
        normalize_http_client_absolute_path(merged_path, query_extra, resolved);
    }
    if (has_selected_query) {
        resolved.push_back('?');
        resolved.append(selected_query.data(), selected_query.size());
    }
    return is_valid_http_origin_form_target(resolved);
}

// The one redirect authority rule: no userinfo, a valid authority, and the
// origin comparison by effective port of the target scheme and URI host
// equivalence.
struct http_client_redirect_authority final {
    std::string_view host_;
    std::uint16_t port_;
    bool same_origin_;
};

[[nodiscard]] std::optional<http_client_redirect_authority> parse_http_client_redirect_authority(
    const http_origin_view& origin, http_scheme target_scheme, std::string_view authority) noexcept {
    if (authority.find('@') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto parsed_value = parse_http_authority(authority);
    if (!parsed_value) {
        return std::nullopt;
    }
    const auto port = parsed_value->effective_port(detail::http_scheme_default_port(target_scheme));
    return http_client_redirect_authority{
        .host_ = parsed_value->host(),
        .port_ = port,
        .same_origin_ = target_scheme == origin.scheme() && port == origin.port() &&
                        detail::http_uri_host_equals(parsed_value->host(), origin.host()),
    };
}

}  // namespace

http_origin_view http_origin_view::http(http_origin_options options) {
    const auto host = options.host_.view();
    validate_origin_host(host);
    return http_origin_view(http_scheme::http, host, resolved_origin_port(http_scheme::http, options));
}

http_origin_view http_origin_view::https(http_origin_options options) {
    const auto host = options.host_.view();
    validate_origin_host(host);
    return http_origin_view(
        http_scheme::https, host, resolved_origin_port(http_scheme::https, options));
}

std::string make_http_origin_authority(const http_origin_view& origin) {
    const auto host = origin.host();
    const bool include_port = !http_origin_uses_default_port(origin);
    std::string authority;
    authority.reserve(host.size() + (include_port ? 6 : 0));
    authority.append(host);
    if (include_port) {
        authority.push_back(':');
        http_append_unsigned(authority, origin.port());
    }
    return authority;
}

http_client_response_header_lookup lookup_unique_http_client_response_header(
    const http_client_response_head& head, std::string_view name) noexcept {
    std::string_view found;
    bool seen = false;
    for (const auto& header : head.headers()) {
        if (!http_ascii_equals_ignore_case(header.name(), name)) {
            continue;
        }
        if (seen) {
            return {.status_ = http_unique_header_status::repeated};
        }
        seen = true;
        found = header.value();
    }
    if (!seen) {
        return {.status_ = http_unique_header_status::absent};
    }
    return {.status_ = http_unique_header_status::found, .value_ = found};
}

http_client_redirect_request_plan::http_client_redirect_request_plan(
    std::string_view method, http_client_redirect_content_disposition content_disposition)
    : method_(method),
      content_disposition_(content_disposition) {}

http_client_redirect_request_plan plan_http_client_redirect_request(
    const http_client_request_view& request, http_client_redirect_request_plan_options options) {
    if (options.status_ == http_status::see_other) {
        return http_client_redirect_request_plan(
            request.method_ == "HEAD" ? request.method_.view() : std::string_view("GET"),
            http_client_redirect_content_disposition::drop);
    }
    if ((options.status_ == http_status::moved_permanently ||
            options.status_ == http_status::found) &&
        request.method_ == "POST") {
        return http_client_redirect_request_plan(
            "GET", http_client_redirect_content_disposition::drop);
    }
    return http_client_redirect_request_plan(
        request.method_.view(), http_client_redirect_content_disposition::preserve);
}

http_client_origin_authority_status classify_http_client_origin_authority(
    const http_origin_view& origin, std::string_view authority) noexcept {
    const auto parsed_value = parse_http_client_redirect_authority(origin, origin.scheme(), authority);
    if (!parsed_value) {
        return http_client_origin_authority_status::invalid_authority;
    }
    return parsed_value->same_origin_ ? http_client_origin_authority_status::same_origin
                                      : http_client_origin_authority_status::different_origin;
}

http_origin_view http_client_resolved_redirect::origin() const& {
    return scheme_ == http_scheme::https ? http_origin_view::https({.host_ = host(), .port_ = port_})
                                         : http_origin_view::http({.host_ = host(), .port_ = port_});
}

std::variant<http_client_resolved_redirect, http_client_redirect_resolution_error>
resolve_http_client_redirect_target(
    const http_origin_view& origin, http_client_redirect_target_options options) {
    const auto current_target = options.current_target_.view();
    auto location = options.location_.view();
    if (!is_valid_http_origin_form_target(current_target)) {
        return http_client_redirect_resolution_error::invalid_current_target;
    }

    location = http_trim_ows(location);
    if (const auto hash = location.find('#'); hash != std::string_view::npos) {
        if (!detail::is_valid_uri_component(location.substr(hash + 1), true, true)) {
            return http_client_redirect_resolution_error::invalid_location;
        }
        location = location.substr(0, hash);
    }

    bool has_authority = false;
    auto target_scheme = origin.scheme();
    std::string_view reference = location;
    const auto colon = reference.find(':');
    const auto first_path_or_query = reference.find_first_of("/?");
    if (colon != std::string_view::npos &&
        (first_path_or_query == std::string_view::npos || colon < first_path_or_query)) {
        const auto scheme = reference.substr(0, colon);
        if (!detail::is_valid_uri_scheme(scheme)) {
            return http_client_redirect_resolution_error::invalid_location;
        }
        const bool is_https = http_ascii_equals_ignore_case(scheme, "https");
        if (!is_https && !http_ascii_equals_ignore_case(scheme, "http")) {
            return http_client_redirect_resolution_error::unsupported_scheme;
        }
        target_scheme = is_https ? http_scheme::https : http_scheme::http;
        reference.remove_prefix(colon + 1);
        if (!reference.starts_with("//")) {
            return http_client_redirect_resolution_error::invalid_location;
        }
        reference.remove_prefix(2);
        has_authority = true;
    } else if (reference.starts_with("//")) {
        reference.remove_prefix(2);
        has_authority = true;
    }

    std::string_view resolved_host = origin.host();
    auto port = origin.port();
    bool cross_origin = target_scheme != origin.scheme();
    if (has_authority) {
        const auto authority_end = reference.find_first_of("/?");
        const auto authority =
            authority_end == std::string_view::npos ? reference : reference.substr(0, authority_end);
        const auto parsed_value = parse_http_client_redirect_authority(origin, target_scheme, authority);
        if (!parsed_value) {
            return http_client_redirect_resolution_error::invalid_location;
        }
        resolved_host = parsed_value->host_;
        port = parsed_value->port_;
        cross_origin = !parsed_value->same_origin_;
        reference = authority_end == std::string_view::npos ? std::string_view{}
                                                            : reference.substr(authority_end);
    }

    std::string resolved;
    if (!resolve_http_client_redirect_path_and_query(
            has_authority, reference, current_target, resolved)) {
        return http_client_redirect_resolution_error::invalid_location;
    }

    std::string host(resolved_host.data(), resolved_host.size());
    return http_client_resolved_redirect(
        target_scheme, std::move(host), port, std::move(resolved), cross_origin);
}

}  // namespace ruvia

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
    if (value.empty() || !is_uri_alpha(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (const auto c : value.substr(1)) {
        const auto byte = static_cast<unsigned char>(c);
        if (!is_uri_alpha(byte) && !is_decimal_digit(c) && byte != '+' && byte != '-' &&
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

namespace ruvia::detail {
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
        return std::ranges::all_of(
            value.substr(2), [](char digit) noexcept { return http_decode_hex_nibble(digit) >= 0; });
    }
    if (value.size() >= 2 && value.front() == '0') {
        return std::ranges::all_of(
            value.substr(1), [](char digit) noexcept { return digit >= '0' && digit <= '7'; });
    }
    return false;
}

[[nodiscard]] bool serialized_origin_domain_ends_in_number(std::string_view domain) noexcept {
    if (!domain.empty() && domain.back() == '.') {
        domain.remove_suffix(1);
    }
    const auto separator = domain.rfind('.');
    const auto last_label =
        separator == std::string_view::npos ? domain : domain.substr(separator + 1);
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

[[nodiscard]] std::size_t find_serialized_origin_ipv6_compression_index(
    const std::array<std::uint16_t, 8>& pieces) noexcept {
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

[[nodiscard]] bool append_serialized_origin_ipv6_hex(
    std::array<char, 39>& output, std::size_t& size, std::uint16_t value) noexcept {
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

[[nodiscard]] bool append_serialized_origin_ipv6_byte(
    std::array<char, 39>& output, std::size_t& size, char byte) noexcept {
    if (size == output.size()) {
        return false;
    }
    output[size++] = byte;
    return true;
}

[[nodiscard]] bool is_canonical_serialized_origin_ipv6(
    std::string_view literal, const std::array<std::uint16_t, 8>& pieces) noexcept {
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
        if (piece_index != pieces.size() - 1 &&
            !append_serialized_origin_ipv6_byte(serialized, size, ':')) {
            return false;
        }
    }

    return literal.size() == size &&
           std::equal(serialized.begin(), serialized.begin() + static_cast<std::ptrdiff_t>(size),
               literal.begin());
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

[[nodiscard]] std::optional<std::uint16_t> serialized_origin_default_port(
    std::string_view scheme) noexcept {
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

}  // namespace ruvia

namespace ruvia {
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

[[nodiscard]] bool uri_reference_keeps_literal(
    unsigned char byte, std::size_t index, const uri_reference_layout& layout, bool& fragment_seen) noexcept {
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
                const bool can_reuse = (initially_empty || reference.size() <= storage.capacity() / 3) &&
                                       !detail::http_text_aliases_string(reference, storage);
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

std::optional<std::string_view> decode_url_component(
    borrowed_text text, std::string& storage, url_decode_mode mode) {
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
            const bool can_write_in_place =
                (mode == url_decode_mode::form || storage.empty()) &&
                !detail::http_text_aliases_string(input, storage);
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

[[nodiscard]] bool parse_ipv6_hex_group(std::string_view literal, std::size_t& offset,
    std::uint16_t& group) noexcept {
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
[[nodiscard]] bool is_valid_percent_encoded(
    std::string_view value, is_allowed_type is_allowed) noexcept {
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

bool split_uri_host_port(
    std::string_view value, std::string_view& host, std::string_view& port, bool& has_port) noexcept {
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

[[nodiscard]] bool is_valid_uri_component(
    std::string_view value, bool allow_slash, bool allow_question) noexcept {
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
        if (is_decimal_digit(literal[offset]) && next_dot != std::string_view::npos &&
            (next_colon == std::string_view::npos || next_dot < next_colon)) {
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
    return !value.empty() &&
           is_valid_percent_encoded(value, [](unsigned char byte) noexcept {
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

namespace ruvia {

// Owns only newly staged descriptors, never copies the existing header block.
// Staging and reservation may throw and leave the block unchanged; publication
// transfers every descriptor without allocation; unwinding releases the rest.
class http_response_headers::descriptor_stage final {
public:
    explicit descriptor_stage(http_response_headers& owner_value) noexcept
        : owner_(owner_value) {}
    descriptor_stage(const descriptor_stage&) = delete;
    descriptor_stage& operator=(const descriptor_stage&) = delete;

    ~descriptor_stage() noexcept {
        for (std::size_t slot = 0; slot < count_; ++slot) {
            owner_.release_header(prepared_[slot]);
        }
    }

    // Each staged field must be distinct.
    void stage(std::string_view name, std::string_view value, std::uint32_t known_bit) {
        const auto builtin = make_static_header(name, value, known_bit);
        prepared_[count_] = builtin ? *builtin : owner_.make_owned_header(name, value, known_bit);
        ++count_;
    }

    void stage_weak_etag(std::string_view strong_etag) {
        auto prepared = owner_.make_uninitialized_header(
            "ETag", strong_etag.size() + 2U, detail::response_header_etag);
        auto* const out = detail::http_response_header_access::value_begin(prepared);
        out[0] = 'W';
        out[1] = '/';
        std::memcpy(out + 2, strong_etag.data(), strong_etag.size());
        prepared_[count_++] = prepared;
    }

    // Reserve a table slot for every staged field the block lacks, so publish
    // cannot allocate. Removing fields before publish keeps the reservation valid.
    void reserve() {
        std::size_t missing_headers = 0;
        for (std::size_t slot = 0; slot < count_; ++slot) {
            const auto& prepared = prepared_[slot];
            if (owner_.find_header_for_read(prepared.name(), detail::http_response_header_access::known_bit(prepared)) ==
                nullptr) {
                ++missing_headers;
            }
        }
        owner_.reserve(owner_.size() + missing_headers);
    }

    // Replace each field in place, collapsing earlier appended lines, or append it.
    void publish() noexcept {
        for (std::size_t slot = 0; slot < count_; ++slot) {
            // reserve() made room for every appended field, so this cannot throw.
            owner_.publish_header(prepared_[slot]);
        }
        count_ = 0;
    }

private:
    static constexpr std::size_t capacity = 3;

    http_response_headers& owner_;
    std::array<http_response_header, capacity> prepared_{};
    std::size_t count_{0};
};

std::string_view http_response::body_bytes() const& noexcept {
    return detail::response_body(*this).bytes();
}

std::optional<http_response_file_view> http_response::file_body() const& noexcept {
    return detail::response_body(*this).file();
}

bool http_response::has_multipart_file_body() const noexcept {
    return detail::response_body(*this).multipart_body() != nullptr;
}

bool http_response::has_file_representation() const noexcept {
    return detail::response_body(*this).file_representation();
}

void http_response::head_metadata_body() noexcept {
    body_.set_head_metadata_only();
}

bool http_response::has_head_metadata_body() const noexcept {
    return body_.head_metadata_only();
}

std::size_t http_response::body_segment_count() const noexcept {
    const auto& body = detail::response_body(*this);
    if (const auto* multipart = body.multipart_body()) {
        return multipart->segment_count();
    }
    return body.file().has_value() || body.size() != 0 ? 1 : 0;
}

http_response_body_segment_view http_response::body_segment(std::size_t index) const& {
    const auto& body = detail::response_body(*this);
    if (const auto* multipart = body.multipart_body()) {
        if (index >= multipart->segment_count()) {
            throw std::out_of_range("response body segment index is out of range");
        }
        return multipart->segment(index);
    }
    if (index != 0) {
        throw std::out_of_range("response body segment index is out of range");
    }
    if (const auto file = body.file()) {
        return {.file_ = file};
    }
    return {.bytes_ = body.bytes()};
}

std::uint64_t http_response_body_plan::buffered_representation_length(const http_response& response) const noexcept {
    if (!status_allows_body() || content_semantics() == http_response_content_semantics::connect_tunnel) {
        return 0;
    }
    return static_cast<std::uint64_t>(detail::response_body(response).size());
}

bool http_buffered_response_write_plan::matches_response(const http_response& response) const noexcept {
    return response.status() == body_plan_.response_status() &&
           content_length_ == body_plan_.buffered_representation_length(response);
}

http_buffered_response_write_plan plan_buffered_http_response_write(
    http_known_method request_method, const http_response& response) noexcept {
    const auto body_plan = plan_http_response_body(request_method, response.status());
    return http_buffered_response_write_plan(body_plan, body_plan.buffered_representation_length(response));
}

http_response_body_plan plan_http_response_body(
    http_known_method request_method, http_status_code response_status) noexcept {
    const bool body_allowed = !response_status.is_informational() &&
                              response_status != http_status::no_content &&
                              response_status != http_status::reset_content &&
                              response_status != http_status::not_modified;
    const bool auto_content_length_allowed = body_allowed || response_status == http_status::reset_content;
    const bool explicit_content_length_allowed = body_allowed || response_status == http_status::not_modified;
    const auto semantics = detail::classify_http_response_content_semantics(request_method, response_status);
    const bool connect_tunnel = semantics == http_response_content_semantics::connect_tunnel;
    return http_response_body_plan(request_method, response_status, semantics, body_allowed,
        !body_allowed || semantics != http_response_content_semantics::with_content,
        !connect_tunnel && auto_content_length_allowed,
        !connect_tunnel && explicit_content_length_allowed,
        !connect_tunnel && body_allowed);
}

http_response::http_response() noexcept = default;

http_response::http_response(http_response&& other) noexcept
    : status_code_(other.status_code_),
      headers_(std::move(other.headers_)),
      body_(std::move(other.body_)) {}

http_response& http_response::operator=(http_response&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    http_response retired(std::move(*this));
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
    return *this;
}

void http_response::commit_headers(http_response_headers&& staged) noexcept {
    headers_.commit(std::move(staged));
}

http_status_code http_response::status() const noexcept {
    return status_code_;
}

const http_response_headers& http_response::headers() const& noexcept {
    return headers_;
}

std::optional<std::string_view> http_response::header(std::string_view name) const& noexcept {
    return headers_.header(name);
}

void http_response::header(std::string_view key, std::string_view value) {
    headers_.header(key, value);
}

void http_response::header(std::string_view key, std::string_view value, http_response_header_options options) {
    headers_.header(key, value, options);
}

void http_response::header_stable_view(std::string_view key, std::string_view value) {
    headers_.header_stable_view(key, value);
}

void http_response::content_type(http_content_type type) {
    headers_.content_type(type);
}

void http_response::allow_methods(
    std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    headers_.allow_methods(method_mask, extension_methods);
}

void http_response::remove_header(std::string_view key) {
    headers_.remove_header(key);
}

void http_response::set_cookie(const set_cookie_plan& plan) {
    headers_.set_cookie(plan);
}

void http_response::reserve_headers(std::size_t count) {
    headers_.reserve(count);
}

void http_response::status(http_status_code status_code) {
    if (status_code == http_status::switching_protocols) {
        throw std::invalid_argument("Switching Protocols requires a dedicated protocol driver");
    }
    if (!status_code.is_final()) {
        throw std::invalid_argument("invalid final HTTP status code");
    }
    status_code_ = status_code;
}

void http_response::transfer_headers_from(
    const http_response& source_value, http_response_header_transfer mode) {
    headers_.transfer_from(source_value.headers_, mode);
}

void http_response::body(std::string_view value) {
    body_.set_copy(value);
}

void http_response::owned_body(std::string&& value) noexcept {
    body_.set_owned(std::move(value));
}

void http_response::static_body(std::string_view value) noexcept {
    body_.set_static(value);
}

void http_response::file_bytes_body(std::string&& value) noexcept {
    body_.set_file_bytes(std::move(value));
}

void http_response::apply_content_encoding(std::string_view content_encoding) {
    apply_encoded_representation(content_encoding, nullptr);
}

void http_response::replace_body_with_content_encoding(
    std::string&& value, std::string_view content_encoding) {
    apply_encoded_representation(content_encoding, &value);
}

void http_response::apply_encoded_representation(
    std::string_view content_encoding, std::string* body) {
    detail::validate_response_header_storage_size(std::string_view("Content-Encoding").size(), content_encoding.size());
    // Coding-list parsing permits field-line OWS; publish normalized field
    // content so the same response can be emitted by HTTP/1 and HTTP/2.
    content_encoding = http_trim_ows(content_encoding);
    detail::response_field_state fields;
    if (detail::validate_response_field(detail::http_field_kind::content_encoding, content_encoding,
            fields) != detail::response_field_status::ok) {
        throw std::invalid_argument("invalid HTTP Content-Encoding header");
    }
    // RFC 9110 §8.8.3: the coded representation is not byte-identical to the
    // identity one, so a strong validator becomes weak. A value that is not an
    // entity-tag cannot be weakened unambiguously and is dropped (nginx gzip
    // filter, Pingora weaken_or_clear_etag).
    const auto current_etag = detail::http_response_header_state_access::known_header_value(headers_, detail::response_header_etag);
    const auto etag_opaque = detail::http_trim_weak_etag_prefix(current_etag);
    const bool etag_well_formed = etag_opaque.size() >= 2 && etag_opaque.front() == '"' && etag_opaque.back() == '"';
    http_response_headers::descriptor_stage update(headers_);
    update.stage("Content-Encoding", content_encoding, detail::response_header_content_encoding);
    if (etag_well_formed && !detail::http_is_weak_etag(current_etag)) {
        update.stage_weak_etag(current_etag);
    }
    std::array<char, http_max_unsigned_decimal_size> length_digits;
    if (body != nullptr) {
        update.stage("Content-Length", http_format_unsigned(length_digits, body->size()),
            detail::response_header_content_length);
    }
    update.reserve();
    // Every descriptor and table slot exists before the body changes. set_owned
    // has a strong failure guarantee; everything after it is no-throw publication.
    if (body != nullptr) {
        body_.set_owned(std::move(*body));
    } else {
        if (body_.size() == 0 && !body_.file_representation()) {
            head_metadata_body();
        }
        (void)headers_.remove_header_validated("Content-Length", detail::response_header_content_length);
    }
    if (!etag_well_formed) {
        (void)headers_.remove_header_validated("ETag", detail::response_header_etag);
    }
    // Byte ranges of the identity representation do not address the coded
    // one, and the framework never serves ranges of a body it encoded
    // (RFC 9110 §14.3).
    (void)headers_.remove_header_validated("Accept-Ranges", detail::response_header_accept_ranges);
    update.publish();
}

void http_response::file_body(http_native_path_view_type file, http_content_length size,
    http_content_length offset, http_content_length length, http_response_file_identity identity,
    http_response_file_descriptor opened) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset.value() > size.value() || length.value() > size.value() - offset.value()) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.set_owned_file(file, size.value(), offset.value(), length.value(),
        identity, std::move(opened));
}

void http_response::multipart_file_body(http_native_path_view_type file, http_content_length size,
    http_response_file_identity identity, http_multipart_byte_range_plan&& plan,
    http_response_file_descriptor opened) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    body_.set_multipart(
        file, size.value(), identity, std::move(plan), std::move(opened));
}

void http_response::content_range(
    http_content_length offset, http_content_length length, http_content_length size) {
    headers_.content_range(offset, length, size);
}

void http_response::content_range_unsatisfied(http_content_length size) {
    headers_.content_range_unsatisfied(size);
}

void http_response::add_vary_token(std::string_view token) {
    headers_.add_vary_token(token);
}

}  // namespace ruvia

namespace ruvia {
namespace {

constexpr std::string_view crlf = "\r\n";

struct http1_interim_header_facts final {
    http_field_section_budget section_;
    detail::http_connection_options connection_options_;
    detail::http_upgrade_protocols upgrade_protocols_;
};

[[nodiscard]] const char* common_validation_message(detail::http_interim_response_header_validation_status status) noexcept {
    switch (status) {
        case detail::http_interim_response_header_validation_status::content_length_forbidden:
            return "Content-Length is forbidden on an interim response";
        case detail::http_interim_response_header_validation_status::transfer_encoding_forbidden:
            return "Transfer-Encoding is forbidden on an interim response";
        case detail::http_interim_response_header_validation_status::trailer_forbidden:
            return "Trailer is forbidden on an interim response";
        case detail::http_interim_response_header_validation_status::repeated_singleton:
            return "repeated singleton interim response header";
        case detail::http_interim_response_header_validation_status::invalid_header:
        case detail::http_interim_response_header_validation_status::ok:
            break;
    }
    return "invalid HTTP/1 interim response header";
}

void analyze_http1_fields(const http_interim_response_head& response, http1_interim_header_facts& facts) {
    detail::http_interim_response_sender_validator validator;
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        const auto value = header.value();
        auto kind = detail::http_field_kind::other;
        const auto status = validator.validate(name, value, &kind);
        if (status != detail::http_interim_response_header_validation_status::ok) {
            throw http1_response_head_error(common_validation_message(status));
        }
        switch (detail::parse_http_response_control_field(
            kind, value, facts.connection_options_, facts.upgrade_protocols_)) {
            case detail::http_response_control_field_status::invalid_connection:
                throw http1_response_head_error("invalid HTTP/1 interim response Connection field");
            case detail::http_response_control_field_status::invalid_upgrade:
                throw http1_response_head_error("invalid HTTP/1 interim response Upgrade field");
            case detail::http_response_control_field_status::te_forbidden:
                throw http1_response_head_error("TE is not a response field");
            case detail::http_response_control_field_status::ok:
                break;
        }

        if (!facts.section_.add_field(name, value)) {
            throw http1_response_head_error("HTTP/1 interim response head is too large");
        }
    }
    if (!detail::http_response_upgrade_pair_valid(facts.connection_options_, facts.upgrade_protocols_)) {
        throw http1_response_head_error("HTTP/1 interim Upgrade requires Connection: Upgrade");
    }
}

[[nodiscard]] char* write_interim_response_head(
    char* output, const http_interim_response_head& response, std::string_view reason_phrase) noexcept {
    detail::http_raw_wire_sink sink(output);
    detail::emit_http1_status_line(sink, response.status(), reason_phrase);
    for (const auto& header : response.headers()) {
        detail::emit_http1_field_line(sink, header.name(), header.value());
    }
    sink.append(crlf);
    return sink.position();
}

}  // namespace

http_interim_response_head::http_interim_response_head(
    http_status_code status_code, borrowed_http_headers headers)
    : status_code_(status_code),
      headers_(headers.view()) {
    if (!detail::http_interim_status_code_valid(status_code)) {
        throw std::invalid_argument(status_code == http_status::switching_protocols
                                        ? "Switching Protocols requires a dedicated protocol driver"
                                        : "invalid interim HTTP status code");
    }
}

std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small>
prepare_http1_interim_response_head(const http_interim_response_head& response, std::span<char> head_buffer) {
    const auto reason_phrase = http_reason_phrase(response.status());
    http1_interim_header_facts facts;
    analyze_http1_fields(response, facts);
    if (!facts.section_.add(detail::http1_status_line_size(reason_phrase)) || !facts.section_.add(crlf.size())) {
        throw http1_response_head_error("HTTP/1 interim response head is too large");
    }
    const auto head_bytes = facts.section_.bytes();
    if (head_buffer.size() < head_bytes) {
        return http1_interim_response_buffer_too_small(head_bytes);
    }

    const auto output = head_buffer.first(head_bytes);
    detail::write_http_wire(output,
        detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), response.headers()),
        [&](char* destination) noexcept {
            return write_interim_response_head(destination, response, reason_phrase);
        });

    return prepared_http1_interim_response(std::string_view(head_buffer.data(), head_bytes),
        facts.connection_options_.close()
            ? http1_interim_connection_disposition::close_after_interim_response
            : http1_interim_connection_disposition::unchanged);
}

}  // namespace ruvia

namespace ruvia {

const http_representation_response_plan::full_type* http_representation_response_plan::full() const& noexcept {
    return std::get_if<full_type>(&value_);
}

const http_representation_response_plan::not_modified_type*
http_representation_response_plan::not_modified() const& noexcept {
    return std::get_if<not_modified_type>(&value_);
}

const http_representation_response_plan::precondition_failed_type*
http_representation_response_plan::precondition_failed() const& noexcept {
    return std::get_if<precondition_failed_type>(&value_);
}

const http_representation_response_plan::range_unsatisfiable_type*
http_representation_response_plan::range_unsatisfiable() const& noexcept {
    return std::get_if<range_unsatisfiable_type>(&value_);
}

const http_representation_response_plan::partial_type* http_representation_response_plan::partial() const& noexcept {
    return std::get_if<partial_type>(&value_);
}

const http_representation_response_plan::multipart_type*
http_representation_response_plan::multipart_ranges() const& noexcept {
    return std::get_if<multipart_type>(&value_);
}

http_status_code http_representation_response_plan::status() const noexcept {
    if (const auto* outcome = full()) {
        return outcome->status_;
    }
    if (not_modified() != nullptr) {
        return http_status::not_modified;
    }
    if (precondition_failed() != nullptr) {
        return http_status::precondition_failed;
    }
    if (range_unsatisfiable() != nullptr) {
        return http_status::range_not_satisfiable;
    }
    return http_status::partial_content;
}

http_representation_response_plan plan_http_representation_response(
    const http_request& request, http_selected_representation_metadata representation,
    http_representation_response_options options) {
    switch (options.range_policy_) {
        case http_range_request_policy::ignore:
        case http_range_request_policy::honor_byte_ranges:
            break;
        default:
            throw std::invalid_argument("unknown HTTP range request policy");
    }

    const auto make_full = [&]() noexcept {
        return http_representation_response_plan(http_representation_response_plan::full_type{options.normal_status_});
    };
    auto method = get_http_conditional_method_plan(request.known_method());
    if (request.known_method() == http_known_method::unknown &&
        !request.method().empty() && request.method() != "TRACE") {
        method.evaluates_preconditions_ = true;
    }
    if (!method.evaluates_preconditions_ ||
        (!options.normal_status_.is_successful() && options.normal_status_ != http_status::precondition_failed)) {
        return make_full();
    }

    const auto headers = get_http_conditional_headers(request);
    const auto etags = get_http_etag_preconditions(request, representation.etag_);
    if (etags.if_match_.present_) {
        if (!etags.if_match_.matches()) {
            return http_representation_response_plan(
                http_representation_response_plan::precondition_failed_type{});
        }
    } else if (representation.last_modified_ && !headers.if_unmodified_since_.empty() &&
               !http_date_unmodified(headers.if_unmodified_since_, *representation.last_modified_)) {
        return http_representation_response_plan(http_representation_response_plan::precondition_failed_type{});
    }

    if (etags.if_none_match_.present_) {
        if (etags.if_none_match_.matches()) {
            if (method.uses_not_modified_response_) {
                return http_representation_response_plan(http_representation_response_plan::not_modified_type{});
            }
            return http_representation_response_plan(http_representation_response_plan::precondition_failed_type{});
        }
    } else if (method.evaluates_if_modified_since_ && representation.last_modified_ &&
               !headers.if_modified_since_.empty() &&
               http_date_not_modified(headers.if_modified_since_, *representation.last_modified_)) {
        return http_representation_response_plan(http_representation_response_plan::not_modified_type{});
    }

    if (options.normal_status_ != http_status::ok ||
        !method.evaluates_range_ ||
        options.range_policy_ != http_range_request_policy::honor_byte_ranges || headers.range_.empty()) {
        return make_full();
    }

    if (headers.has_if_range_ && !http_if_range_allows(headers.if_range_, representation.etag_,
                                     representation.last_modified_, representation.strong_date_validator_)) {
        return make_full();
    }

    const auto ranges = resolve_http_byte_range_set(headers.range_, representation.length_);
    if (ranges.ignored()) {
        return make_full();
    }
    if (ranges.unsatisfiable()) {
        return http_representation_response_plan(
            http_representation_response_plan::range_unsatisfiable_type{});
    }
    if (ranges.size() == 1) {
        return http_representation_response_plan(ranges[0], headers.has_if_range_);
    }
    if (representation.content_coded_) {
        return make_full();
    }
    return http_representation_response_plan(ranges, headers.has_if_range_);
}

}  // namespace ruvia

namespace ruvia {

[[nodiscard]] http_response_stream_head prepare_http_response_stream_head(
    http_response response, http_response_stream_commit_plan commit_plan) {
    if (response.status() != commit_plan.response_status()) {
        throw std::invalid_argument("response stream commit plan status does not match response");
    }
    if (!commit_plan.trailer_intent_allowed()) {
        throw std::invalid_argument("response stream status does not permit trailers");
    }
    const auto framing = commit_plan.framing();
    if (!detail::response_transfer_encoding_replaceable(response)) {
        if (framing == http_response_stream_framing::http2_frames ||
            framing == http_response_stream_framing::http3_frames) {
            throw std::invalid_argument("HTTP/2 or HTTP/3 response cannot carry a Transfer-Encoding other than chunked");
        }
        throw http1_response_head_error("HTTP response Transfer-Encoding must be exactly chunked");
    }
    const auto body_plan = commit_plan.body_plan();
    const bool writer_owns_http1_chunked = framing == http_response_stream_framing::http1_chunked &&
                                           body_plan.transfer_encoding_allowed() &&
                                           !body_plan.body_suppressed();

    // Keep the prepared response metadata consistent with the wire plan. The
    // framework's chunk writer is the only Transfer-Encoding producer, so the
    // handler's chunked hint survives only as that writer's own field; HTTP/2
    // and HTTP/3 frames and an HTTP/1.0 close-delimited body carry none. An HTTP/1.0
    // close-delimited body cannot retain Content-Length either. HEAD/304 may
    // retain Content-Length metadata because their body is suppressed.
    if (writer_owns_http1_chunked) {
        response.remove_header("Content-Length");
    } else {
        response.remove_header("Transfer-Encoding");
    }
    if (framing == http_response_stream_framing::http1_close_delimited && !body_plan.body_suppressed()) {
        response.remove_header("Content-Length");
    }
    // Without a chunked body the trailer section is discarded, so the head must
    // not announce trailer fields that will never follow (RFC 9110 6.6.2).
    if (!commit_plan.trailers_available()) {
        response.remove_header("Trailer");
    }

    if (writer_owns_http1_chunked) {
        if (!detail::response_has_known_header(response, detail::response_header_transfer_encoding)) {
            response.reserve_headers(response.headers().size() + 1);
        }
        response.header_stable_view("Transfer-Encoding", "chunked");
    }

    return http_response_stream_head(std::move(response), commit_plan);
}

}  // namespace ruvia

namespace ruvia {

http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
    const http_byte_range_set& ranges, http_content_length representation_length_value,
    std::string_view media_type, std::string_view boundary) {
    const auto representation_length = representation_length_value.value();
    if (ranges.ignored() || ranges.unsatisfiable() || ranges.size() < 2 ||
        ranges.size() > http_byte_range_set::capacity || representation_length == 0 ||
        !is_valid_http_content_type_field_value(media_type) ||
        !multipart_boundary::try_create(boundary).has_value()) {
        throw std::invalid_argument("invalid multipart byte-range plan input");
    }

    const auto representation_digits = http_unsigned_decimal_size(representation_length);
    std::size_t metadata_size = 2U + boundary.size() + 4U;
    std::uint64_t total_content_length = metadata_size;
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto range = ranges[index];
        if (range.length_ == 0 || range.offset_ > representation_length ||
            range.length_ > representation_length - range.offset_) {
            throw std::invalid_argument("multipart range is outside the representation");
        }
        const auto part_metadata_size =
            2U + boundary.size() + 16U + media_type.size() + 23U +
            http_unsigned_decimal_size(range.offset_) + 1U +
            http_unsigned_decimal_size(range.offset_ + range.length_ - 1) + 1U +
            representation_digits + 6U;
        if (part_metadata_size > (std::numeric_limits<std::size_t>::max)() - metadata_size) {
            throw std::length_error("multipart response metadata size overflows size_t");
        }
        metadata_size += part_metadata_size;
        if (total_content_length > (std::numeric_limits<std::uint64_t>::max)() - part_metadata_size ||
            total_content_length + part_metadata_size >
                (std::numeric_limits<std::uint64_t>::max)() - range.length_) {
            throw std::length_error("multipart response content length overflows uint64_t");
        }
        total_content_length += part_metadata_size + range.length_;
    }

    http_multipart_byte_range_plan plan;
    auto& storage = *plan.state_;
    auto& metadata = storage.metadata_;
    static constexpr std::string_view content_type_prefix = "multipart/byteranges; boundary=";
    const bool boundary_is_token = http_token_prefix_size(boundary) == boundary.size();
    storage.content_type_.reserve(
        content_type_prefix.size() + boundary.size() + (boundary_is_token ? 0U : 2U));
    storage.content_type_.append(content_type_prefix);
    if (!boundary_is_token) {
        storage.content_type_.push_back('"');
    }
    storage.content_type_.append(boundary);
    if (!boundary_is_token) {
        storage.content_type_.push_back('"');
    }
    metadata.reserve(metadata_size);
    const auto record_metadata = [&storage](std::size_t offset) noexcept {
        const auto length = storage.metadata_.size() - offset;
        if (storage.segment_count_ != 0) {
            auto& previous = storage.segments_[storage.segment_count_ - 1];
            if (previous.kind_ == http_multipart_byte_range_plan::segment_kind::metadata &&
                previous.metadata_offset_ + previous.metadata_length_ == offset) {
                previous.metadata_length_ = storage.metadata_.size() - previous.metadata_offset_;
                return;
            }
        }
        storage.segments_[storage.segment_count_++] = {
            http_multipart_byte_range_plan::segment_kind::metadata, offset, length, 0, 0};
    };

    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto range = ranges[index];
        const auto part_offset = metadata.size();
        metadata.append("--");
        metadata.append(boundary);
        metadata.append("\r\nContent-Type: ");
        metadata.append(media_type);
        metadata.append("\r\nContent-Range: bytes ");
        http_append_unsigned(metadata, range.offset_);
        metadata.push_back('-');
        http_append_unsigned(metadata, range.offset_ + range.length_ - 1);
        metadata.push_back('/');
        http_append_unsigned(metadata, representation_length);
        metadata.append("\r\n\r\n");
        record_metadata(part_offset);

        storage.segments_[storage.segment_count_++] = {
            http_multipart_byte_range_plan::segment_kind::file, 0, 0, range.offset_, range.length_};
        const auto separator_offset = metadata.size();
        metadata.append("\r\n");
        record_metadata(separator_offset);
    }

    const auto closing_offset = metadata.size();
    metadata.append("--");
    metadata.append(boundary);
    metadata.append("--\r\n");
    record_metadata(closing_offset);
    storage.content_length_ = total_content_length;
    return plan;
}

http_multipart_byte_range_plan http_multipart_byte_range_plan::clone() const {
    if (state_ == nullptr) {
        throw std::invalid_argument("multipart response plan must contain segments");
    }
    http_multipart_byte_range_plan copy;
    copy.state_->content_type_.assign(state_->content_type_);
    copy.state_->metadata_.assign(state_->metadata_);
    copy.state_->segments_ = state_->segments_;
    copy.state_->segment_count_ = state_->segment_count_;
    copy.state_->content_length_ = state_->content_length_;
    return copy;
}

http_byte_range_set resolve_http_byte_range_set(std::string_view field_value,
    http_content_length representation_length_value, http_byte_range_options options) {
    const auto representation_length = representation_length_value.value();
    http_byte_range_set result;
    if (options.max_ranges_ == 0) {
        return result;
    }
    field_value = http_trim_ows(field_value);
    constexpr std::string_view unit = "bytes";
    if (field_value.size() <= unit.size() || field_value[unit.size()] != '=' ||
        !http_ascii_equals_ignore_case(field_value.substr(0, unit.size()), unit)) {
        return result;
    }

    const auto parse_number = [](std::string_view digits, std::uint64_t& value) noexcept {
        if (digits.empty()) {
            return false;
        }
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (end != digits.data() + digits.size()) {
            return false;
        }
        if (error == std::errc::result_out_of_range) {
            value = (std::numeric_limits<std::uint64_t>::max)();
            return true;
        }
        return error == std::errc{};
    };

    const bool strict = options.strict_order_ ||
                        options.order_policy_ == http_byte_range_order_policy::strict_ascending_disjoint;
    const auto add_range = [&](std::uint64_t offset, std::uint64_t length) -> bool {
        if (strict) {
            if (result.count_ > 0) {
                const auto prev = result[result.count_ - 1];
                if (offset < prev.offset_ + prev.length_) {
                    return false;
                }
            }
            if (result.count_ == options.max_ranges_) {
                return false;
            }
            result.append_slot({offset, length});
            return true;
        }
        if (result.count_ == options.max_ranges_) {
            return false;
        }
        auto start = offset;
        auto end = offset + length;
        auto target = result.count_;
        std::size_t kept = 0;
        for (std::size_t index = 0; index < result.count_; ++index) {
            const auto current = result[index];
            const auto current_end = current.offset_ + current.length_;
            if (current.offset_ <= end && start <= current_end) {
                start = (std::min)(start, current.offset_);
                end = (std::max)(end, current_end);
                if (target == result.count_) {
                    target = kept++;
                }
                continue;
            }
            result.mutable_at(kept++) = current;
        }
        if (target == result.count_) {
            result.append_slot({start, end - start});
        } else {
            result.resize_slots(kept);
            result.mutable_at(target) = {start, end - start};
        }
        return true;
    };

    auto specs = http_trim_ows(field_value.substr(unit.size() + 1));
    if (specs.empty()) {
        return result;
    }
    result.ignored_ = false;
    std::size_t empty_members = 0;
    bool saw_range_spec = false;
    bool has_more = false;
    do {
        const auto comma = specs.find(',');
        auto spec = http_trim_ows(specs.substr(0, comma));
        has_more = comma != std::string_view::npos;
        specs = has_more ? specs.substr(comma + 1) : std::string_view{};
        if (spec.empty()) {
            if (++empty_members > 32) {
                return http_byte_range_set{};
            }
            continue;
        }
        saw_range_spec = true;
        const auto dash = spec.find('-');
        if (dash == std::string_view::npos || spec.find('-', dash + 1) != std::string_view::npos) {
            return http_byte_range_set{};
        }
        const auto first = spec.substr(0, dash);
        const auto last = spec.substr(dash + 1);
        std::uint64_t start{};
        std::uint64_t end{};
        if (first.empty()) {
            std::uint64_t suffix{};
            if (!parse_number(last, suffix)) {
                return http_byte_range_set{};
            }
            if (suffix == 0 || representation_length == 0) {
                continue;
            }
            const auto length = (std::min)(suffix, representation_length);
            if (!add_range(representation_length - length, length)) {
                return http_byte_range_set{};
            }
            continue;
        }
        if (!parse_number(first, start) || (!last.empty() && !parse_number(last, end))) {
            return http_byte_range_set{};
        }
        if (!last.empty()) {
            if (start > end) {
                return http_byte_range_set{};
            }
            if (start == (std::numeric_limits<std::uint64_t>::max)() && end == start) {
                auto normalized_first = first;
                auto normalized_last = last;
                while (normalized_first.size() > 1 && normalized_first.front() == '0') {
                    normalized_first.remove_prefix(1);
                }
                while (normalized_last.size() > 1 && normalized_last.front() == '0') {
                    normalized_last.remove_prefix(1);
                }
                if (normalized_first.size() > normalized_last.size() ||
                    (normalized_first.size() == normalized_last.size() &&
                        normalized_first > normalized_last)) {
                    return http_byte_range_set{};
                }
            }
        }
        if (start >= representation_length) {
            continue;
        }
        const auto clamped_end =
            last.empty() ? representation_length - 1 : (std::min)(end, representation_length - 1);
        if (!add_range(start, clamped_end - start + 1)) {
            return http_byte_range_set{};
        }
    } while (has_more);
    if (result.count_ == 0 && !saw_range_spec) {
        return http_byte_range_set{};
    }
    return result;
}

std::optional<http_content_range> parse_http_content_range(std::string_view field_value) noexcept {
    field_value = http_trim_ows(field_value);
    constexpr std::string_view prefix = "bytes ";
    if (field_value.size() <= prefix.size() ||
        !http_ascii_equals_ignore_case(field_value.substr(0, 5), "bytes") ||
        (field_value[5] != ' ' && field_value[5] != '\t')) {
        return std::nullopt;
    }
    const auto spec = http_trim_ows(field_value.substr(5));
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto slash = spec.find('/', dash + 1);
    if (slash == std::string_view::npos || spec.find('/', slash + 1) != std::string_view::npos ||
        spec.find('-', dash + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto parse_exact = [](std::string_view digits, std::uint64_t& out) noexcept {
        if (digits.empty()) {
            return false;
        }
        const auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), out);
        return err == std::errc{} && end == digits.data() + digits.size();
    };
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::uint64_t total = 0;
    if (!parse_exact(spec.substr(0, dash), first) ||
        !parse_exact(spec.substr(dash + 1, slash - dash - 1), last) ||
        !parse_exact(spec.substr(slash + 1), total)) {
        return std::nullopt;
    }
    if (first > last || last >= total) {
        return std::nullopt;
    }
    return http_content_range{
        .first_ = first,
        .last_ = last,
        .total_ = total,
    };
}

std::optional<std::uint64_t> compute_http_byte_range_content_length(
    std::span<const http_byte_range> ranges, http_content_length representation_length_value,
    std::string_view media_type, std::string_view boundary) noexcept {
    const auto representation_length = representation_length_value.value();
    if (ranges.empty() || representation_length == 0) {
        return std::nullopt;
    }
    for (const auto& range : ranges) {
        if (range.length_ == 0 || range.offset_ > representation_length ||
            range.length_ > representation_length - range.offset_) {
            return std::nullopt;
        }
    }
    if (ranges.size() == 1) {
        return ranges.front().length_;
    }
    if (!is_valid_http_content_type_field_value(media_type) ||
        !multipart_boundary::try_create(boundary).has_value()) {
        return std::nullopt;
    }
    const auto representation_digits = http_unsigned_decimal_size(representation_length);
    std::uint64_t total = 2U + boundary.size() + 4U;
    for (const auto& range : ranges) {
        const std::uint64_t part_metadata =
            2U + boundary.size() + 16U + media_type.size() + 23U +
            http_unsigned_decimal_size(range.offset_) + 1U +
            http_unsigned_decimal_size(range.offset_ + range.length_ - 1) + 1U +
            representation_digits + 6U;
        constexpr auto max_u64 = (std::numeric_limits<std::uint64_t>::max)();
        if (total > max_u64 - part_metadata ||
            total + part_metadata > max_u64 - range.length_) {
            return std::nullopt;
        }
        total += part_metadata + range.length_;
    }
    return total;
}

std::optional<std::uint64_t> compute_http_byte_range_content_length(
    const http_byte_range_set& ranges, http_content_length representation_length,
    std::string_view media_type, std::string_view boundary) noexcept {
    if (ranges.ignored() || ranges.unsatisfiable()) {
        return std::nullopt;
    }
    return compute_http_byte_range_content_length(
        ranges.ranges(), representation_length, media_type, boundary);
}

http_byte_range_slicer::http_byte_range_slicer(const http_byte_range_set& ranges,
    http_content_length representation_length, std::string_view media_type,
    std::string_view boundary)
    : http_byte_range_slicer(
          ranges.ignored() ? std::span<const http_byte_range>{} : ranges.ranges(),
          representation_length, media_type, boundary) {}

http_byte_range_slicer::http_byte_range_slicer(std::span<const http_byte_range> ranges,
    http_content_length representation_length_value, std::string_view media_type,
    std::string_view boundary)
    : ranges_(ranges.begin(), ranges.end()),
      media_type_(media_type),
      boundary_(boundary),
      content_type_(),
      content_range_(),
      representation_length_(representation_length_value.value()) {
    if (ranges_.empty() || representation_length_ == 0) {
        throw std::invalid_argument("HTTP byte-range slicer requires at least one range");
    }
    for (std::size_t index = 0; index < ranges_.size(); ++index) {
        const auto range = ranges_[index];
        if (range.length_ == 0 || range.offset_ > representation_length_ ||
            range.length_ > representation_length_ - range.offset_) {
            throw std::invalid_argument("HTTP byte range is outside representation");
        }
        if (index > 0) {
            const auto prev = ranges_[index - 1];
            if (range.offset_ < prev.offset_ + prev.length_) {
                throw std::invalid_argument(
                    "HTTP byte-range slicer requires ascending non-overlapping ranges");
            }
        }
    }
    if (ranges_.size() == 1) {
        if (!media_type.empty() && !is_valid_http_content_type_field_value(media_type)) {
            throw std::invalid_argument("invalid HTTP media type for byte-range slicer");
        }
        content_type_.assign(media_type);
        const auto single = ranges_.front();
        content_range_.append("bytes ");
        http_append_unsigned(content_range_, single.offset_);
        content_range_.push_back('-');
        http_append_unsigned(content_range_, single.offset_ + single.length_ - 1);
        content_range_.push_back('/');
        http_append_unsigned(content_range_, representation_length_);
        content_length_ = single.length_;
        return;
    }
    if (!is_valid_http_content_type_field_value(media_type) ||
        !multipart_boundary::try_create(boundary).has_value()) {
        throw std::invalid_argument("invalid multipart media type or boundary for byte-range slicer");
    }
    const auto computed = compute_http_byte_range_content_length(
        ranges_, representation_length_value, media_type, boundary);
    if (!computed.has_value()) {
        throw std::length_error("multipart byte-range response content length overflows uint64_t");
    }
    content_length_ = *computed;
    static constexpr std::string_view content_type_prefix = "multipart/byteranges; boundary=";
    const bool boundary_is_token = http_token_prefix_size(boundary) == boundary.size();
    content_type_.reserve(
        content_type_prefix.size() + boundary.size() + (boundary_is_token ? 0U : 2U));
    content_type_.append(content_type_prefix);
    if (!boundary_is_token) {
        content_type_.push_back('"');
    }
    content_type_.append(boundary);
    if (!boundary_is_token) {
        content_type_.push_back('"');
    }
}

void http_byte_range_slicer::emit_part_header(
    std::size_t index, std::string& output) const {
    const auto range = ranges_[index];
    output.append("--");
    output.append(boundary_);
    output.append("\r\nContent-Type: ");
    output.append(media_type_);
    output.append("\r\nContent-Range: bytes ");
    http_append_unsigned(output, range.offset_);
    output.push_back('-');
    http_append_unsigned(output, range.offset_ + range.length_ - 1);
    output.push_back('/');
    http_append_unsigned(output, representation_length_);
    output.append("\r\n\r\n");
}

void http_byte_range_slicer::write(std::string_view chunk, std::string& output) {
    write(current_offset_, chunk, output);
}

void http_byte_range_slicer::write(
    std::uint64_t chunk_offset, std::string_view chunk, std::string& output) {
    if (chunk_offset < current_offset_ || chunk_offset > representation_length_ ||
        chunk.size() > representation_length_ - chunk_offset) {
        throw std::invalid_argument("HTTP byte-range slicer chunk offset is out of bounds");
    }
    if (complete_) {
        current_offset_ = chunk_offset + chunk.size();
        return;
    }
    const auto needed_offset = ranges_[range_index_].offset_ + range_progress_;
    if (chunk_offset > needed_offset) {
        throw std::invalid_argument(
            "HTTP byte-range slicer chunk skipped over required range bytes");
    }
    const std::uint64_t chunk_end = chunk_offset + chunk.size();
    current_offset_ = chunk_end;
    if (chunk.empty()) {
        return;
    }

    while (range_index_ < ranges_.size()) {
        const auto range = ranges_[range_index_];
        const std::uint64_t next_needed = range.offset_ + range_progress_;
        if (chunk_end <= next_needed) {
            break;
        }
        if (range_progress_ == 0 && multipart()) {
            emit_part_header(range_index_, output);
        }
        const std::size_t slice_start = static_cast<std::size_t>(next_needed - chunk_offset);
        const std::uint64_t remaining_in_range = range.length_ - range_progress_;
        const std::uint64_t available_in_chunk = chunk.size() - slice_start;
        const auto copy_bytes =
            static_cast<std::size_t>((std::min)(remaining_in_range, available_in_chunk));
        output.append(chunk.data() + slice_start, copy_bytes);
        range_progress_ += copy_bytes;
        if (range_progress_ == range.length_) {
            if (multipart()) {
                output.append("\r\n");
            }
            ++range_index_;
            range_progress_ = 0;
            if (range_index_ == ranges_.size()) {
                if (multipart()) {
                    output.append("--");
                    output.append(boundary_);
                    output.append("--\r\n");
                }
                complete_ = true;
                break;
            }
        } else {
            break;
        }
    }
}

void http_byte_range_slicer::finish(std::string&) {
    if (!complete_) {
        throw std::logic_error("HTTP byte-range slicer finished before all ranges completed");
    }
}

void http_byte_range_slicer::reset(std::uint64_t start_offset) noexcept {
    current_offset_ = start_offset;
    range_progress_ = 0;
    range_index_ = 0;
    complete_ = false;
}

}  // namespace ruvia

namespace ruvia::detail {

template <std::size_t n>
[[nodiscard]] consteval http_response_header static_response_header(
    std::string_view name, const char (&bytes_value)[n], std::uint32_t known_bit) {
    static_assert(n > 1 && response_header_storage_size_fits(0, n - 1));
    if (name.empty() || name.size() > n - 1 || bytes_value[n - 1] != '\0' ||
        !std::string_view(bytes_value, n - 1).starts_with(name)) {
        throw std::logic_error("invalid static response header storage");
    }
    if (known_bit != 0 && known_bit != response_header_upgrade &&
        response_known_header_slot(known_bit) == response_known_header_count) {
        throw std::logic_error("static response header requires one known header bit");
    }
    return http_response_header_access::make(
        bytes_value, static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(n - 1 - name.size()), known_bit, false);
}

template <std::size_t... sizes>
[[nodiscard]] consteval auto static_response_headers(
    std::string_view name, std::uint32_t known_bit, const char (&... bytes_values)[sizes]) {
    return std::array{static_response_header(name, bytes_values, known_bit)...};
}

[[nodiscard]] inline std::optional<http_response_header> find_static_response_header(
    std::span<const http_response_header> headers, std::string_view value) noexcept {
    for (const auto& header_value : headers) {
        if (header_value.value() == value) {
            return header_value;
        }
    }
    return std::nullopt;
}

inline constexpr auto content_type_headers = static_response_headers("Content-Type", response_header_content_type,
    "Content-Typetext/plain; charset=UTF-8",
    "Content-Typeapplication/json",
    "Content-Typetext/html; charset=UTF-8",
    "Content-Typetext/plain; charset=utf-8",
    "Content-Typeapplication/json; charset=utf-8",
    "Content-Typetext/html; charset=utf-8",
    "Content-Typetext/css; charset=utf-8",
    "Content-Typetext/javascript; charset=utf-8",
    "Content-Typetext/event-stream",
    "Content-Typeimage/png",
    "Content-Typeimage/jpeg",
    "Content-Typeimage/gif",
    "Content-Typeimage/svg+xml",
    "Content-Typeapplication/wasm",
    "Content-Typeapplication/octet-stream",
    "Content-Typeapplication/problem+json",
    "Content-Typeimage/apng",
    "Content-Typeimage/avif",
    "Content-Typeimage/webp",
    "Content-Typeimage/bmp",
    "Content-Typeimage/vnd.microsoft.icon",
    "Content-Typefont/woff",
    "Content-Typefont/woff2",
    "Content-Typefont/ttf",
    "Content-Typefont/otf",
    "Content-Typeapplication/vnd.ms-fontobject",
    "Content-Typetext/xml; charset=utf-8",
    "Content-Typeapplication/manifest+json; charset=utf-8");

[[nodiscard]] inline http_response_header static_content_type_header(http_content_type type) {
    switch (type) {
        case http_content_type::text_plain_utf8:
            return content_type_headers[0];
        case http_content_type::text_html_utf8:
            return content_type_headers[2];
        case http_content_type::application_json:
            return content_type_headers[1];
    }
    throw std::invalid_argument("invalid HTTP content type");
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_response_header(
    std::uint32_t known_bit, std::string_view value) noexcept {
    switch (known_bit) {
        case response_header_content_type:
            return find_static_response_header(content_type_headers, value);
        case response_header_connection: {
            static constexpr auto headers = static_response_headers("Connection", response_header_connection,
                "Connectionclose", "Connectionkeep-alive", "ConnectionUpgrade");
            return find_static_response_header(headers, value);
        }
        case response_header_accept_ranges: {
            static constexpr auto headers = static_response_headers("Accept-Ranges", response_header_accept_ranges,
                "Accept-Rangesbytes");
            return find_static_response_header(headers, value);
        }
        case response_header_content_encoding: {
            static constexpr auto headers = static_response_headers("Content-Encoding", response_header_content_encoding,
                "Content-Encodinggzip", "Content-Encodingbr", "Content-Encodingzstd", "Content-Encodingdeflate");
            return find_static_response_header(headers, value);
        }
        case response_header_transfer_encoding: {
            static constexpr auto headers = static_response_headers("Transfer-Encoding", response_header_transfer_encoding,
                "Transfer-Encodingchunked");
            return find_static_response_header(headers, value);
        }
        case response_header_cache_control: {
            static constexpr auto headers = static_response_headers("Cache-Control", response_header_cache_control,
                "Cache-Controlno-store", "Cache-Controlno-cache", "Cache-Controlprivate");
            return find_static_response_header(headers, value);
        }
        case response_header_vary: {
            static constexpr auto headers = static_response_headers("Vary", response_header_vary,
                "Vary*", "VaryAccept-Encoding", "VaryOrigin", "VaryAccess-Control-Request-Headers", "VaryAccess-Control-Request-Method");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_origin: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Origin", response_header_access_control_allow_origin,
                "Access-Control-Allow-Origin*");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_credentials: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Credentials", response_header_access_control_allow_credentials,
                "Access-Control-Allow-Credentialstrue");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_methods: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Methods", response_header_access_control_allow_methods,
                "Access-Control-Allow-MethodsGET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
            return find_static_response_header(headers, value);
        }
        case response_header_upgrade: {
            static constexpr auto headers = static_response_headers("Upgrade", response_header_upgrade,
                "Upgradewebsocket", "Upgradeconnect-udp");
            return find_static_response_header(headers, value);
        }
        default:
            return std::nullopt;
    }
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_custom_response_header(
    std::string_view name, std::string_view value) noexcept {
    switch (name.size()) {
        case std::string_view("Retry-After").size():
            if (name == "Retry-After") {
                static constexpr auto headers = static_response_headers("Retry-After", 0, "Retry-After1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-Frame-Options").size():
            if (name == "X-Frame-Options") {
                static constexpr auto headers = static_response_headers("X-Frame-Options", 0,
                    "X-Frame-OptionsDENY", "X-Frame-OptionsSAMEORIGIN");
                return find_static_response_header(headers, value);
            }
            if (name == "Referrer-Policy") {
                static constexpr auto headers = static_response_headers("Referrer-Policy", 0,
                    "Referrer-Policystrict-origin-when-cross-origin", "Referrer-Policyno-referrer");
                return find_static_response_header(headers, value);
            }
            if (name == "Accept-Encoding") {
                static constexpr auto headers = static_response_headers("Accept-Encoding", 0,
                    "Accept-Encodinggzip, deflate, br, zstd", "Accept-Encodingidentity");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-XSS-Protection").size():
            if (name == "X-XSS-Protection") {
                static constexpr auto headers = static_response_headers("X-XSS-Protection", 0, "X-XSS-Protection0");
                return find_static_response_header(headers, value);
            }
            if (name == "Capsule-Protocol") {
                static constexpr auto headers = static_response_headers("Capsule-Protocol", 0, "Capsule-Protocol?1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-RateLimit-Reset").size():
            if (name == "X-RateLimit-Reset") {
                static constexpr auto headers = static_response_headers("X-RateLimit-Reset", 0, "X-RateLimit-Reset1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Permissions-Policy").size():
            if (name == "Permissions-Policy") {
                static constexpr auto headers = static_response_headers("Permissions-Policy", 0,
                    "Permissions-Policygeolocation=(), microphone=(), camera=()");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-RateLimit-Remaining").size():
            if (name == "X-RateLimit-Remaining") {
                static constexpr auto headers = static_response_headers("X-RateLimit-Remaining", 0, "X-RateLimit-Remaining0");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-Content-Type-Options").size():
            if (name == "X-Content-Type-Options") {
                static constexpr auto headers = static_response_headers("X-Content-Type-Options", 0, "X-Content-Type-Optionsnosniff");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Content-Security-Policy").size():
            if (name == "Content-Security-Policy") {
                static constexpr auto headers = static_response_headers("Content-Security-Policy", 0,
                    "Content-Security-Policydefault-src 'self'", "Content-Security-Policydefault-src 'none'");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Strict-Transport-Security").size():
            if (name == "Strict-Transport-Security") {
                static constexpr auto headers = static_response_headers("Strict-Transport-Security", 0,
                    "Strict-Transport-Securitymax-age=31536000; includeSubDomains");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

}  // namespace ruvia::detail

namespace ruvia {

http_response_headers::http_response_headers() noexcept = default;

http_response_headers::~http_response_headers() {
    clear();
}

http_response_headers::http_response_headers(http_response_headers&& other) noexcept {
    move_from(std::move(other));
}

void http_response_headers::reserve(std::size_t count) {
    if (count <= inline_capacity) {
        return;
    }

    if (!spilled_) {
        spill(count);
        return;
    }
    if (count <= heap_.capacity()) {
        return;
    }
    // Only descriptors move; field bytes retain their existing ownership.
    std::vector<http_response_header> replacement;
    replacement.reserve(count);
    replacement.insert(replacement.end(), heap_.begin(), heap_.end());
    heap_.swap(replacement);
}

http_response_header& http_response_headers::append_prepared_header(http_response_header header_value) noexcept {
    if (!spilled_) {
        if (size_ == inline_capacity) {
            std::terminate();
        }
        auto* const target = inline_data() + size_;
        *target = header_value;
        ++size_;
        return *target;
    }

    if (heap_.size() == heap_.capacity()) {
        std::terminate();
    }
    heap_.push_back(header_value);
    return heap_.back();
}

http_response_header* http_response_headers::inline_data() noexcept {
    return reinterpret_cast<http_response_header*>(inline_.data());
}

const http_response_header* http_response_headers::inline_data() const noexcept {
    return reinterpret_cast<const http_response_header*>(inline_.data());
}

http_response_header* http_response_headers::data() noexcept {
    return spilled_ ? heap_.data() : inline_data();
}

const http_response_header* http_response_headers::data() const noexcept {
    return spilled_ ? heap_.data() : inline_data();
}

void http_response_headers::clear() noexcept {
    auto* items = data();
    const auto count = size();
    for (std::size_t i = 0; i < count; ++i) {
        release_header(items[i]);
    }
    if (spilled_) {
        heap_.clear();
    }
    size_ = 0;
}

void http_response_headers::spill(std::size_t min_capacity) {
    heap_.clear();
    heap_.reserve(std::max<std::size_t>(inline_capacity * 2, min_capacity));
    auto* items = inline_data();
    for (std::size_t i = 0; i < size_; ++i) {
        heap_.push_back(items[i]);
    }
    size_ = heap_.size();
    spilled_ = true;
}

void http_response_headers::move_from(http_response_headers&& other) noexcept {
    known_header_bits_ = std::exchange(other.known_header_bits_, 0U);
    if (known_header_bits_ != 0) {
        known_header_indexes_ = other.known_header_indexes_;
    }
    if (other.spilled_) {
        spilled_ = true;
        heap_ = std::move(other.heap_);
        size_ = heap_.size();
        other.spilled_ = false;
        other.size_ = 0;
        return;
    }

    if (other.size_ > 0) {
        std::memcpy(inline_.data(), other.inline_.data(), other.size_ * sizeof(inline_storage_type));
    }
    size_ = other.size_;
    other.size_ = 0;
}

bool http_response_headers::borrows_owned_header(std::string_view name, std::string_view value) const noexcept {
    for (const auto& existing : *this) {
        if (existing.owned_ &&
            (detail::response_header_storage_overlaps(existing, name) ||
                detail::response_header_storage_overlaps(existing, value))) {
            return true;
        }
    }
    return false;
}
http_response_header http_response_headers::make_owned_header(
    std::string_view name, std::string_view value, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value.size());
    const auto total = name.size() + value.size();
    char* bytes_value = nullptr;
    if (total > 0) {
        bytes_value = new char[total];
        std::memcpy(bytes_value, name.data(), name.size());
        std::memcpy(bytes_value + name.size(), value.data(), value.size());
    }
    return detail::http_response_header_access::make(bytes_value, static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(value.size()), known_bit, true);
}

http_response_header http_response_headers::make_uninitialized_header(
    std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value_size);
    const auto total = name.size() + value_size;
    char* bytes_value = nullptr;
    if (total > 0) {
        bytes_value = new char[total];
        std::memcpy(bytes_value, name.data(), name.size());
    }
    return detail::http_response_header_access::make(bytes_value, static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(value_size), known_bit, true);
}

std::optional<http_response_header> http_response_headers::make_static_header(
    std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept {
    if (known_bit == 0) {
        return detail::builtin_static_custom_response_header(name, value);
    }
    auto header_value = detail::builtin_static_response_header(known_bit, value);
    if (!header_value || header_value->name() != name) {
        return std::nullopt;
    }
    return header_value;
}

std::optional<http_response_header> http_response_headers::make_shared_header(
    std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view) const noexcept {
    if (const auto static_header = make_static_header(name, value, known_bit)) {
        return static_header;
    }
    if (stable_view && !borrows_owned_header(name, value) &&
        detail::response_header_views_contiguous(name, value) &&
        detail::response_header_storage_size_fits(name.size(), value.size())) {
        return detail::http_response_header_access::make(name.data(),
            static_cast<std::uint32_t>(name.size()), static_cast<std::uint32_t>(value.size()),
            known_bit, false);
    }
    return std::nullopt;
}

void http_response_headers::release_header(http_response_header& header_value) noexcept {
    if (header_value.owned_ && header_value.bytes_ != nullptr) {
        delete[] const_cast<char*>(header_value.bytes_);
    }
    header_value.bytes_ = nullptr;
    header_value.name_size_ = 0;
    header_value.value_size_ = 0;
    header_value.known_bit_ = 0;
    header_value.owned_ = false;
    header_value.append_ = false;
}

void http_response_headers::truncate(iterator new_end) noexcept {
    if (spilled_) {
        heap_.erase(heap_.begin() + (new_end - heap_.data()), heap_.end());
        return;
    }
    size_ = static_cast<std::size_t>(new_end - inline_data());
}

http_response_header& http_response_headers::append_header(http_response_header header_value) {
    // `header` may own a separately allocated name/value block.  The vector
    // stores the small descriptor by value, so an exception while spilling or
    // appending must release that block here; otherwise a failed heap
    // allocation leaks the response header and leaves the next retry with a
    // different ownership picture.
    try {
        if (!spilled_ && size_ == inline_capacity) {
            spill(size_ + 1);
        }
        if (!spilled_) {
            auto* target = inline_data() + size_;
            *target = header_value;
            ++size_;
            return *target;
        }
        heap_.push_back(header_value);
        return heap_.back();
    } catch (...) {
        release_header(header_value);
        throw;
    }
}

http_response_header& http_response_headers::add(
    std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view) {
    const auto shared_header = make_shared_header(name, value, known_bit, stable_view);
    return append_header(shared_header ? *shared_header : make_owned_header(name, value, known_bit));
}

http_response_header& http_response_headers::add_uninitialized_value(
    std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    return append_header(make_uninitialized_header(name, value_size, known_bit));
}

http_response_header& http_response_headers::assign_uninitialized_value(http_response_header& header_value,
    std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value_size);
    const auto total = name.size() + value_size;
    if (header_value.owned_ && header_value.bytes_ != nullptr && !detail::response_header_storage_overlaps(header_value, name) &&
        total == static_cast<std::size_t>(header_value.name_size_) + header_value.value_size_) {
        auto* const bytes_value = const_cast<char*>(header_value.bytes_);
        std::memcpy(bytes_value, name.data(), name.size());
        header_value.name_size_ = static_cast<std::uint32_t>(name.size());
        header_value.value_size_ = static_cast<std::uint32_t>(value_size);
        header_value.known_bit_ = known_bit;
        header_value.append_ = false;
        return header_value;
    }

    const auto replacement = make_uninitialized_header(name, value_size, known_bit);
    release_header(header_value);
    header_value = replacement;
    return header_value;
}

bool http_response_headers::try_assign_owned_in_place(http_response_header& header_value, std::string_view name,
    std::string_view value, std::uint32_t known_bit) noexcept {
    if (!detail::response_header_storage_size_fits(name.size(), value.size())) {
        return false;
    }
    const auto total = name.size() + value.size();
    if (!header_value.owned_ || header_value.bytes_ == nullptr || detail::response_header_storage_overlaps(header_value, name) ||
        detail::response_header_storage_overlaps(header_value, value) ||
        total != static_cast<std::size_t>(header_value.name_size_) + header_value.value_size_) {
        return false;
    }
    auto* const bytes_value = const_cast<char*>(header_value.bytes_);
    std::memcpy(bytes_value, name.data(), name.size());
    std::memcpy(bytes_value + name.size(), value.data(), value.size());
    header_value.name_size_ = static_cast<std::uint32_t>(name.size());
    header_value.value_size_ = static_cast<std::uint32_t>(value.size());
    header_value.known_bit_ = known_bit;
    header_value.append_ = false;
    return true;
}

void http_response_headers::assign(http_response_header& header_value, std::string_view name,
    std::string_view value, std::uint32_t known_bit, bool stable_view) {
    if (const auto shared_header = make_shared_header(name, value, known_bit, stable_view)) {
        release_header(header_value);
        header_value = *shared_header;
        return;
    }
    if (try_assign_owned_in_place(header_value, name, value, known_bit)) {
        return;
    }
    const auto replacement = make_owned_header(name, value, known_bit);
    release_header(header_value);
    header_value = replacement;
}

namespace {

inline constexpr std::size_t allow_header_method_slots = static_cast<std::size_t>(http_known_method::unknown);

void write_content_range_header_value(
    http_response_header& header_value, std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    sink.append("bytes ");
    sink.append_unsigned(offset);
    sink.append('-');
    sink.append_unsigned(offset + length - 1);
    sink.append('/');
    sink.append_unsigned(size);
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

void write_content_range_unsatisfied_header_value(http_response_header& header_value, std::uint64_t size) {
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    sink.append("bytes */");
    sink.append_unsigned(size);
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

[[nodiscard]] std::size_t allow_header_value_size(
    std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    std::size_t size = 0;
    std::size_t count = 0;
    const auto add_method = [&](std::size_t token_size) {
        const auto separator_size = count == 0 ? std::size_t{0} : std::size_t{2};
        detail::validate_response_header_storage_size(std::string_view("Allow").size() + size, separator_size);
        size += separator_size;
        detail::validate_response_header_storage_size(std::string_view("Allow").size() + size, token_size);
        size += token_size;
        ++count;
    };
    for (std::size_t i = 0; i < allow_header_method_slots; ++i) {
        if ((method_mask & (1U << i)) == 0) {
            continue;
        }
        add_method(known_http_method_token(static_cast<http_known_method>(i)).size());
    }
    for (const auto token : extension_methods) {
        add_method(token.size());
    }
    return size;
}

void write_allow_header_value(http_response_header& header_value, std::uint32_t method_mask,
    std::span<const std::string_view> extension_methods) {
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    bool first = true;
    for (std::size_t i = 0; i < allow_header_method_slots; ++i) {
        if ((method_mask & (1U << i)) == 0) {
            continue;
        }
        if (!first) {
            sink.append(", ");
        }
        first = false;
        sink.append(known_http_method_token(static_cast<http_known_method>(i)));
    }
    for (const auto token : extension_methods) {
        if (!first) {
            sink.append(", ");
        }
        first = false;
        sink.append(token);
    }
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Allow header");
    }
}

[[nodiscard]] bool set_cookie_wire_name_matches(
    std::string_view value, std::string_view wire_prefix, std::string_view cookie_name) noexcept {
    if (value.size() != wire_prefix.size() + cookie_name.size()) {
        return false;
    }
    if (!value.starts_with(wire_prefix)) {
        return false;
    }
    return value.substr(wire_prefix.size()) == cookie_name;
}

[[nodiscard]] bool set_cookie_value_matches_storage_key(std::string_view value,
    std::string_view wire_prefix, std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain, bool partitioned) noexcept {
    const auto parsed_value = parse_set_cookie(value);
    return parsed_value.has_value() && is_valid_http_header_name(parsed_value->name()) &&
           set_cookie_wire_name_matches(parsed_value->name(), wire_prefix, cookie_name) &&
           !parsed_value->path().empty() == has_path &&
           parsed_value->has(http_set_cookie_attribute::partitioned) == partitioned &&
           (!has_path || parsed_value->path() == path) &&
           http_ascii_equals_ignore_case(parsed_value->domain(), domain);
}

}  // namespace

void http_response_headers::allow_methods(
    std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    const auto value_size = allow_header_value_size(method_mask, extension_methods);
    for (const auto token : extension_methods) {
        if (!is_valid_http_method_token(token)) {
            throw std::invalid_argument("invalid HTTP Allow method");
        }
    }
    if (!extension_methods.empty()) {
        if (auto* const retained = find_header_for_update("Allow", detail::response_header_allow)) {
            bool borrows_allow = false;
            const auto overlaps_retained = [&](std::string_view token) noexcept {
                if (!detail::http_response_header_access::append(*retained)) {
                    return detail::response_header_storage_overlaps(*retained, token);
                }
                for (const auto& existing : *this) {
                    if (detail::http_response_header_access::known_bit(existing) == detail::response_header_allow &&
                        detail::response_header_storage_overlaps(existing, token)) {
                        return true;
                    }
                }
                return false;
            };
            for (const auto token : extension_methods) {
                if (overlaps_retained(token)) {
                    borrows_allow = true;
                    break;
                }
            }
            if (borrows_allow) {
                auto prepared = make_uninitialized_header(
                    "Allow", value_size, detail::response_header_allow);
                try {
                    write_allow_header_value(prepared, method_mask, extension_methods);
                } catch (...) {
                    release_header(prepared);
                    throw;
                }
                (void)replace_header(*retained, prepared);
                return;
            }
        }
    }
    auto& header_value = prepare_header_value_storage(
        "Allow", value_size, detail::response_header_allow);
    write_allow_header_value(header_value, method_mask, extension_methods);
}

void http_response_headers::content_range(
    http_content_length offset_value, http_content_length length_value, http_content_length size_value) {
    const auto offset = offset_value.value();
    const auto length = length_value.value();
    const auto size = size_value.value();
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }
    if (offset > size || length > size - offset) {
        throw std::logic_error("file response byte range is outside the representation");
    }
    const auto end_offset = offset + length - 1;
    const auto value_size =
        std::string_view("bytes ").size() + http_unsigned_decimal_size(offset) + 1 +
        http_unsigned_decimal_size(end_offset) + 1 + http_unsigned_decimal_size(size);
    auto& header_value =
        prepare_header_value_storage("Content-Range", value_size, detail::response_header_content_range);
    write_content_range_header_value(header_value, offset, length, size);
}

void http_response_headers::content_range_unsatisfied(http_content_length size_value) {
    const auto size = size_value.value();
    const auto value_size =
        std::string_view("bytes */").size() + http_unsigned_decimal_size(size);
    auto& header_value =
        prepare_header_value_storage("Content-Range", value_size, detail::response_header_content_range);
    write_content_range_unsatisfied_header_value(header_value, size);
}

void http_response_headers::set_cookie(const set_cookie_plan& plan) {
    auto* retained = find_set_cookie_header(
        plan.wire_prefix(), plan.name(), !plan.path().empty(), plan.path(), plan.domain(), plan.partitioned_);
    if (retained == nullptr) {
        auto& header_value = append_header_uninitialized_value(
            "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        plan.write(detail::http_response_header_access::value_begin(header_value));
        return;
    }

    const bool borrows_retained =
        detail::response_header_storage_overlaps(*retained, plan.name_) ||
        detail::response_header_storage_overlaps(*retained, plan.value_) ||
        detail::response_header_storage_overlaps(*retained, plan.path_) ||
        detail::response_header_storage_overlaps(*retained, plan.domain_);
    set_cookie_plan::written_fields written;
    if (borrows_retained) {
        auto prepared = make_uninitialized_header(
            "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        written = plan.write_fields(detail::http_response_header_access::value_begin(prepared));
        release_header(*retained);
        *retained = prepared;
    } else {
        assign_uninitialized_value(
            *retained, "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        written = plan.write_fields(detail::http_response_header_access::value_begin(*retained));
    }
    detail::http_response_header_access::set_append(*retained);
    erase_later_set_cookie_headers(*retained, written.wire_name_, !written.path_.empty(),
        written.path_, written.domain_, plan.partitioned_);
}

void http_response_headers::upsert_set_cookie_header_validated(std::string_view value) {
    const auto parsed_value = parse_set_cookie(value);
    const auto cookie_name = parsed_value.has_value() && is_valid_http_header_name(parsed_value->name())
                                 ? parsed_value->name()
                                 : std::string_view{};
    if (cookie_name.empty()) {
        append_header_validated("Set-Cookie", value, detail::response_header_set_cookie);
        return;
    }

    const auto has_path = !parsed_value->path().empty();
    const auto partitioned = parsed_value->has(http_set_cookie_attribute::partitioned);
    auto* retained = find_set_cookie_header({}, cookie_name, has_path, parsed_value->path(), parsed_value->domain(), partitioned);
    if (retained == nullptr) {
        append_header_validated("Set-Cookie", value, detail::response_header_set_cookie);
        return;
    }

    const auto name_size = cookie_name.size();
    const auto path_size = parsed_value->path().size();
    const auto domain_size = parsed_value->domain().size();
    const auto name_offset = static_cast<std::size_t>(cookie_name.data() - value.data());
    const auto path_offset = parsed_value->path().empty() ? std::size_t{0}
                                                          : static_cast<std::size_t>(parsed_value->path().data() - value.data());
    const auto domain_offset = parsed_value->domain().empty() ? std::size_t{0}
                                                              : static_cast<std::size_t>(parsed_value->domain().data() - value.data());
    assign(*retained, "Set-Cookie", value, detail::response_header_set_cookie);
    detail::http_response_header_access::set_append(*retained);
    const auto copied_value = retained->value();
    erase_later_set_cookie_headers(*retained, copied_value.substr(name_offset, name_size), has_path,
        copied_value.substr(path_offset, path_size), copied_value.substr(domain_offset, domain_size), partitioned);
}

http_response_header* http_response_headers::find_set_cookie_header(std::string_view wire_prefix,
    std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain, bool partitioned) noexcept {
    for (auto* header = mutable_begin(); header != mutable_end(); ++header) {
        if (detail::http_response_header_access::known_bit(*header) == detail::response_header_set_cookie &&
            set_cookie_value_matches_storage_key(
                header->value(), wire_prefix, cookie_name, has_path, path, domain, partitioned)) {
            return header;
        }
    }
    return nullptr;
}

void http_response_headers::erase_later_set_cookie_headers(http_response_header& retained,
    std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain, bool partitioned) noexcept {
    auto* const end_value = mutable_end();
    auto* write = &retained + 1;
    for (auto* read = write; read != end_value; ++read) {
        if (detail::http_response_header_access::known_bit(*read) == detail::response_header_set_cookie &&
            set_cookie_value_matches_storage_key(
                read->value(), {}, cookie_name, has_path, path, domain, partitioned)) {
            release_header(*read);
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != end_value) {
        truncate(write);
        rebuild_known_header_index();
    }
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Every insertion path validates one field line completely, so a serialized
// head only re-checks the rules that span several lines. Returns the field's
// known bit.
[[nodiscard]] std::uint32_t validate_response_header_field(std::string_view key, std::string_view value) {
    // Check the descriptor's representable storage before any grammar scan;
    // callers may provide a view over a bounded buffer with a hostile length.
    detail::validate_response_header_storage_size(key.size(), value.size());
    if (!is_valid_http_header_name(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!is_valid_http_header_value(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto field = detail::classify_http_field_name(key);
    const auto known_bit = field.response_bit_;
    const auto kind = field.kind_;
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    switch (detail::parse_http_response_control_field(kind, value, connection_options, upgrade_protocols)) {
        case detail::http_response_control_field_status::invalid_connection:
            throw std::invalid_argument("invalid HTTP Connection header");
        case detail::http_response_control_field_status::invalid_upgrade:
            throw std::invalid_argument("invalid HTTP Upgrade header");
        case detail::http_response_control_field_status::te_forbidden:
            throw std::invalid_argument("TE is not a response field");
        case detail::http_response_control_field_status::ok:
            break;
    }
    detail::response_field_state fields;
    if (detail::validate_response_field(kind, value, fields) != detail::response_field_status::ok) {
        throw std::invalid_argument("invalid HTTP response field value");
    }
    return known_bit;
}

[[nodiscard]] bool response_header_mode_appends(http_response_header_mode mode) {
    switch (mode) {
        case http_response_header_mode::replace:
            return false;
        case http_response_header_mode::append:
            return true;
    }
    throw std::invalid_argument("invalid HTTP response header mode");
}

}  // namespace

http_response_headers http_response_headers::clone(std::size_t additional_headers) const {
    const auto existing = size();
    if (additional_headers > heap_.max_size() - existing) {
        throw std::length_error("HTTP response header table is too large");
    }
    http_response_headers copy;
    copy.reserve(existing + additional_headers);
    for (const auto& header_value : *this) {
        // Static descriptors can be shared; owning descriptors must keep an
        // independent allocation so either block may be changed or destroyed.
        auto descriptor = header_value.owned_
                              ? copy.make_owned_header(header_value.name(), header_value.value(), header_value.known_bit_)
                              : header_value;
        descriptor.append_ = header_value.append_;
        (void)copy.append_prepared_header(descriptor);
    }
    copy.known_header_bits_ = known_header_bits_;
    if (known_header_bits_ != 0) {
        copy.known_header_indexes_ = known_header_indexes_;
    }
    return copy;
}

void http_response_headers::commit(http_response_headers&& staged) noexcept {
    if (this == &staged) {
        return;
    }
    // Reconstruction adopts the staged table without allocating; the move
    // leaves `staged` empty with its own resource and a cleared index.
    std::destroy_at(this);
    std::construct_at(this, std::move(staged));
}

bool http_response_headers::try_append_disjoint(
    const http_response_headers& source, http_response_header_transfer mode) {
    if (&source == this) {
        return false;
    }
    const auto skips = [mode](std::uint32_t known_bit) noexcept {
        return mode == http_response_header_transfer::assign && known_bit == detail::response_header_content_type;
    };
    // A source block already holds each field under its own rules: one line per
    // single-valued field and key-unique cookies. When no source field names a
    // field this block holds, every transfer mode keeps each source line as is.
    for (const auto& header_value : source) {
        const auto known_bit = detail::http_response_header_access::known_bit(header_value);
        if (skips(known_bit)) {
            continue;
        }
        if (known_bit != 0 ? (known_header_bits_ & known_bit) != 0
                           : find_header_for_read(header_value.name(), 0) != nullptr) {
            return false;
        }
    }

    reserve(size() + source.size());
    const auto first_index = size();
    try {
        for (const auto& header_value : source) {
            const auto known_bit = header_value.known_bit_;
            if (skips(known_bit)) {
                continue;
            }
            // Static descriptors are shared; owning descriptors get an
            // independent allocation, as in clone().
            auto descriptor = header_value.owned_
                                  ? make_owned_header(header_value.name(), header_value.value(), known_bit)
                                  : header_value;
            // Transferred cookies are appended occurrences, like the staged path.
            descriptor.append_ = header_value.append_ || known_bit == detail::response_header_set_cookie;
            (void)append_prepared_header(descriptor);
        }
    } catch (...) {
        auto* const first = data() + first_index;
        for (auto* cursor_value = first; cursor_value != end(); ++cursor_value) {
            release_header(*cursor_value);
        }
        truncate(first);
        throw;
    }
    const auto* const items = data();
    for (auto index = first_index; index < size(); ++index) {
        if (const auto known_bit = items[index].known_bit_; known_bit != 0) {
            record_known_header_index(known_bit, index);
        }
    }
    return true;
}

void http_response_headers::transfer_from(
    const http_response_headers& source, http_response_header_transfer mode) {
    switch (mode) {
        case http_response_header_transfer::merge:
        case http_response_header_transfer::assign:
        case http_response_header_transfer::apply:
            break;
        default:
            throw std::invalid_argument("invalid HTTP response header transfer mode");
    }
    if (try_append_disjoint(source, mode)) {
        return;
    }
    // Fields that meet existing ones are resolved on a staged copy, so a
    // failure leaves this block unchanged.
    auto staged = clone(source.size());
    const auto original_size = staged.size();
    bool removed_cookies = false;
    for (const auto& header_value : source) {
        const auto known_bit = detail::http_response_header_access::known_bit(header_value);
        if (mode == http_response_header_transfer::assign &&
            known_bit == detail::response_header_content_type) {
            continue;
        }
        const auto name = header_value.name();
        const auto value = header_value.value();
        if (known_bit == detail::response_header_set_cookie) {
            if (mode == http_response_header_transfer::assign && !removed_cookies) {
                (void)staged.remove_header_validated("Set-Cookie", detail::response_header_set_cookie);
                removed_cookies = true;
            }
            staged.upsert_set_cookie_header_validated(value);
        } else if (detail::http_response_header_access::append(header_value)) {
            const auto matches_header = [&](const http_response_header& candidate) noexcept {
                return detail::response_header_matches(candidate, name, known_bit) && candidate.value() == value;
            };
            const auto occurrence_through = [&] {
                std::size_t count = 0;
                for (const auto& candidate : source) {
                    if (matches_header(candidate)) {
                        ++count;
                    }
                    if (&candidate == &header_value) {
                        break;
                    }
                }
                return count;
            }();
            const auto existing_count = [&] {
                std::size_t count = 0;
                const auto* const items = staged.data();
                for (std::size_t index = 0, limit = std::min(original_size, staged.size());
                    index < limit; ++index) {
                    if (matches_header(items[index])) {
                        ++count;
                    }
                }
                return count;
            }();
            if (existing_count < occurrence_through) {
                staged.append_header_validated(name, value, known_bit);
            }
        } else if (mode == http_response_header_transfer::merge) {
            if (staged.find_header_for_read(name, known_bit) == nullptr) {
                if (!header_value.owned_) {
                    staged.publish_header(header_value);
                } else {
                    staged.set_header_validated(name, value, known_bit);
                }
            }
        } else if (!header_value.owned_) {
            staged.publish_header(header_value);
        } else {
            staged.set_header_validated(name, value, known_bit);
        }
    }
    commit(std::move(staged));
}

void http_response_headers::record_known_header_index(std::uint32_t known_bit, std::size_t index) noexcept {
    if (known_bit == 0 || (known_header_bits_ & known_bit) != 0) {
        return;
    }
    known_header_bits_ |= known_bit;
    const auto slot = detail::response_known_header_slot(known_bit);
    if (slot >= known_header_indexes_.size()) {
        return;
    }
    // A slot holds the first occurrence's index + 1; 0 (unset) and -1 (index
    // beyond int16) both fall back to a scan.
    known_header_indexes_[slot] = index < static_cast<std::size_t>((std::numeric_limits<std::int16_t>::max)())
                                      ? static_cast<std::int16_t>(index + 1)
                                      : std::int16_t{-1};
}

http_response_header* http_response_headers::find_header_for_update(
    std::string_view key, std::uint32_t known_bit) noexcept {
    return const_cast<http_response_header*>(std::as_const(*this).find_header_for_read(key, known_bit));
}

const http_response_header* http_response_headers::find_header_for_read(
    std::string_view key, std::uint32_t known_bit) const noexcept {
    if (known_bit != 0 && (known_header_bits_ & known_bit) == 0) {
        return nullptr;
    }
    if (const auto slot = detail::response_known_header_slot(known_bit);
        slot < known_header_indexes_.size() && known_header_indexes_[slot] > 0) {
        return begin() + (known_header_indexes_[slot] - 1);
    }
    for (const auto& header_value : *this) {
        if (detail::response_header_matches(header_value, key, known_bit)) {
            return &header_value;
        }
    }
    return nullptr;
}

template <typename Mutation>
http_response_header& http_response_headers::upsert_header(
    std::string_view key, std::uint32_t known_bit, Mutation&& mutate) {
    if (auto* const existing = find_header_for_update(key, known_bit)) {
        const bool was_appended = detail::http_response_header_access::append(*existing);
        auto& header_value = mutate(existing);
        return was_appended ? collapse_response_headers(header_value, known_bit) : header_value;
    }
    const auto index = size();
    auto& header_value = mutate(nullptr);
    record_known_header_index(known_bit, index);
    return header_value;
}

http_response_header& http_response_headers::prepare_header_value_storage(
    std::string_view key, std::size_t value_size, std::uint32_t known_bit) {
    return upsert_header(key, known_bit, [&](http_response_header* existing) -> http_response_header& {
        return existing != nullptr
                   ? assign_uninitialized_value(*existing, key, value_size, known_bit)
                   : add_uninitialized_value(key, value_size, known_bit);
    });
}

std::optional<std::string_view> http_response_headers::header(std::string_view name) const& noexcept {
    if (const auto* const found =
            find_header_for_read(name, detail::classify_response_header_name(name))) {
        return found->value();
    }
    return std::nullopt;
}

void http_response_headers::rebuild_known_header_index() noexcept {
    known_header_bits_ = 0;
    const auto* const first = data();
    const auto* const last = first + size();
    for (auto* cursor_value = first; cursor_value != last; ++cursor_value) {
        const auto known_bit = detail::http_response_header_access::known_bit(*cursor_value);
        if (known_bit != 0) {
            record_known_header_index(known_bit, static_cast<std::size_t>(cursor_value - first));
        }
    }
}

void http_response_headers::header(std::string_view key, std::string_view value) {
    header(key, value, {});
}

void http_response_headers::content_type(http_content_type type) {
    publish_header(detail::static_content_type_header(type));
}

http_response_header& http_response_headers::replace_header(
    http_response_header& existing, http_response_header prepared) noexcept {
    const bool was_appended = detail::http_response_header_access::append(existing);
    release_header(existing);
    existing = prepared;
    return was_appended ? collapse_response_headers(existing, prepared.known_bit_) : existing;
}

void http_response_headers::publish_header(http_response_header prepared) {
    (void)upsert_header(prepared.name(), prepared.known_bit_, [&](http_response_header* existing) -> http_response_header& {
        if (existing != nullptr) {
            release_header(*existing);
            *existing = prepared;
            return *existing;
        }
        return append_header(prepared);
    });
}

void http_response_headers::header(
    std::string_view key, std::string_view value, http_response_header_options options) {
    const auto known_bit = validate_response_header_field(key, value);
    if (response_header_mode_appends(options.mode_)) {
        if (detail::response_header_append_forbidden(known_bit)) {
            throw std::invalid_argument("HTTP response header cannot be appended");
        }
        if (known_bit == detail::response_header_set_cookie) {
            upsert_set_cookie_header_validated(value);
        } else {
            append_header_validated(key, value, known_bit);
        }
    } else {
        set_header_validated(key, value, known_bit);
    }
}

void http_response_headers::remove_header(std::string_view key) {
    detail::validate_response_header_storage_size(key.size(), 0);
    if (!is_valid_http_header_name(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    (void)remove_header_validated(key, detail::classify_response_header_name(key));
}

void http_response_headers::set_header_validated(
    std::string_view key, std::string_view value, std::uint32_t known_bit, bool stable_view) {
    (void)upsert_header(key, known_bit, [&](http_response_header* existing) -> http_response_header& {
        if (existing != nullptr) {
            assign(*existing, key, value, known_bit, stable_view);
            return *existing;
        }
        return add(key, value, known_bit, stable_view);
    });
}

void http_response_headers::append_header_validated(
    std::string_view key, std::string_view value, std::uint32_t known_bit) {
    auto& header_value = append_header_uninitialized_value(key, value.size(), known_bit);
    // Appending can move descriptors, but existing header byte blocks stay
    // stable, including any bytes borrowed by value.
    if (!value.empty()) {
        std::memcpy(detail::http_response_header_access::value_begin(header_value), value.data(), value.size());
    }
}

http_response_header& http_response_headers::append_header_uninitialized_value(
    std::string_view key, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(key.size(), value_size);
    if (detail::response_header_append_forbidden(known_bit)) {
        throw std::invalid_argument("HTTP response header cannot be appended");
    }
    // The index cache intentionally points at the first occurrence. Mark that
    // retained slot as plural too, so a later plain set can detect multiplicity
    // in O(1) and collapse the field without scanning every normal update. Do
    // not set it until the new descriptor has been published: the new header
    // owns bytes before it may allocate the backing table, and a failed append
    // must leave the existing block exactly as it was.
    const auto* const existing = find_header_for_read(key, known_bit);
    const auto existing_index = existing == nullptr
                                    ? std::optional<std::size_t>{}
                                    : std::optional{static_cast<std::size_t>(existing - data())};
    const auto index = size();
    auto& header_value = add_uninitialized_value(key, value_size, known_bit);
    if (existing_index) {
        // Appending may move the descriptor table. Preserve an index, not a pointer,
        // to mark the existing entry without repeating the header lookup.
        detail::http_response_header_access::set_append(data()[*existing_index]);
    }
    // Mark the append flag so a later merge of this block keeps every appended
    // value instead of treating the field as single-valued and dropping all but the
    // first.
    detail::http_response_header_access::set_append(header_value);
    record_known_header_index(known_bit, index);
    return header_value;
}

http_response_header& http_response_headers::collapse_response_headers(
    http_response_header& retained, std::uint32_t known_bit) noexcept {
    const auto key = retained.name();
    auto* const first = data();
    auto* const last = first + size();
    auto* const retained_address = &retained;
    auto* collapsed_retained = retained_address;
    auto* write = first;
    for (auto* read = first; read != last; ++read) {
        if (read != retained_address && detail::response_header_matches(*read, key, known_bit)) {
            release_header(*read);
            continue;
        }
        if (read == retained_address) {
            collapsed_retained = write;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != last) {
        truncate(write);
    }
    rebuild_known_header_index();
    return *collapsed_retained;
}

bool http_response_headers::remove_header_validated(std::string_view key, std::uint32_t known_bit) noexcept {
    if (known_bit != 0 && (known_header_bits_ & known_bit) == 0) {
        return false;
    }
    auto* const first = data();
    auto* const last = first + size();
    auto* write = first;
    bool removed = false;
    http_response_header retired_key_header;

    for (auto* read = first; read != last; ++read) {
        if (detail::response_header_matches(*read, key, known_bit)) {
            if (!removed) {
                retired_key_header = std::exchange(*read, http_response_header{});
                key = retired_key_header.name();
            } else {
                release_header(*read);
            }
            removed = true;
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }

    if (!removed) {
        return false;
    }

    truncate(write);
    rebuild_known_header_index();
    release_header(retired_key_header);
    return true;
}

void http_response_headers::header_stable_view(std::string_view key, std::string_view value) {
    const auto known_bit = validate_response_header_field(key, value);
    set_header_validated(key, value, known_bit, true);
}

void http_response_headers::add_vary_token(std::string_view token) {
    // Vary is a list field (RFC 9110 section 12.5.5): wildcard and membership
    // are decided across every line, not only the indexed first occurrence.
    token = http_trim_ows(token);
    if (token.empty()) {
        return;
    }
    const bool wildcard_token = token == "*";
    if (!wildcard_token && !is_valid_http_header_name(token)) {
        throw std::invalid_argument("invalid HTTP Vary header token");
    }

    std::size_t value_size = token.size();
    constexpr auto max_value_size = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()) - 4;
    detail::validate_response_header_storage_size(4, value_size);
    if ((known_header_bits_ & detail::response_header_vary) != 0) {
        for (const auto& header_value : *this) {
            if (detail::http_response_header_access::known_bit(header_value) != detail::response_header_vary) {
                continue;
            }
            const auto value = header_value.value();
            if (detail::http_has_token(value, "*") || (!wildcard_token && detail::http_has_token(value, token))) {
                return;
            }
            if (!wildcard_token && !http_trim_ows(value).empty()) {
                if (value.size() > max_value_size - value_size ||
                    max_value_size - value_size - value.size() < 2) {
                    throw std::length_error("HTTP response header is too large");
                }
                value_size += value.size() + 2;
            }
        }
    }

    if (wildcard_token) {
        header("Vary", "*");
        return;
    }
    if (value_size == token.size()) {
        header("Vary", token);
        return;
    }

    // Build independently: token may borrow a field that publication retires.
    auto prepared = make_uninitialized_header("Vary", value_size, detail::response_header_vary);
    auto* output = detail::http_response_header_access::value_begin(prepared);
    for (const auto& header_value : *this) {
        if (detail::http_response_header_access::known_bit(header_value) != detail::response_header_vary) {
            continue;
        }
        const auto value = header_value.value();
        if (!http_trim_ows(value).empty()) {
            std::memcpy(output, value.data(), value.size());
            output += value.size();
            *output++ = ',';
            *output++ = ' ';
        }
    }
    std::memcpy(output, token.data(), token.size());
    // Existing lines and token are validated; comma-joining preserves validity.
    publish_header(prepared);
}

}  // namespace ruvia

namespace ruvia {
namespace {

using ruvia::detail::classify_http_request_line_prefix;
using ruvia::detail::find_http_request_head_end;
using ruvia::detail::has_http_bare_line_feed;
using ruvia::detail::http_request_access;
using ruvia::detail::parse_http_header_block;
using ruvia::detail::parse_request_target;
using ruvia::detail::parsed_request_header_block;
using ruvia::detail::request_target_view;

// The head cap hit before the request-line ended. Bytes past the method's SP
// with no further SP or line end are all request-target, which RFC 9112
// section 3 answers with 414 rather than a header-size rejection.
[[nodiscard]] http_parse_error classify_head_cap(std::string_view buffer, std::size_t leading_bytes,
    std::size_t request_line_end) noexcept {
    buffer = buffer.substr(0, ruvia::max_http_header_bytes);
    if (request_line_end != 0 || leading_bytes >= buffer.size()) {
        return http_parse_error::header_too_large;
    }
    const auto line = buffer.substr(leading_bytes);
    const auto method_end = line.find(' ');
    if (method_end == std::string_view::npos ||
        line.find_first_of(" \r\n", method_end + 1) != std::string_view::npos) {
        return http_parse_error::header_too_large;
    }
    return http_parse_error::uri_too_long;
}

void apply_parsed_request_target(http_request& request, const request_target_view& target_view) noexcept {
    http_request_access::set_path(request, target_view.path_);
    http_request_access::set_query_string(request, target_view.query_);
    http_request_access::set_scheme(request, target_view.scheme_);
    http_request_access::set_authority(request, target_view.authority_);
    http_request_access::set_target_form(request, target_view.form_);
}

// RFC 9112 sections 3.2.2 and 3.3 make the request-target authoritative for
// absolute-form and authority-form. Host field lines bind to this value in
// both headers() and the known-header cache so application code cannot
// observe a conflicting Host value as a second routing truth.
[[nodiscard]] std::string_view bound_host_field_value(
    const request_target_view& target_view, std::string_view value) noexcept {
    return target_view.form_ == http_request_target_form::absolute ||
                   target_view.form_ == http_request_target_form::authority
               ? target_view.authority_
               : value;
}

// True if a whitespace-separated word of `line` looks like an HTTP-version:
// "HTTP/" in any case, or token "/" 1*DIGIT "." 1*DIGIT.
[[nodiscard]] bool names_http_version(std::string_view line) noexcept {
    while (!line.empty()) {
        const auto word_end = line.find_first_of(" \t\r\n");
        const auto word = line.substr(0, word_end);
        if (word.size() >= 5 && http_ascii_equals_ignore_case(word.substr(0, 5), "HTTP/")) {
            return true;
        }
        const auto name_size = http_token_prefix_size(word);
        const auto version = word.substr(name_size);
        const auto dot = version.find('.');
        const auto digits = [](std::string_view value) noexcept {
            return !value.empty() && std::ranges::all_of(value, [](char ch) noexcept { return ch >= '0' && ch <= '9'; });
        };
        if (name_size != 0 && version.starts_with('/') && dot != std::string_view::npos &&
            digits(version.substr(1, dot - 1)) && digits(version.substr(dot + 1))) {
            return true;
        }
        if (word_end == std::string_view::npos) {
            return false;
        }
        line.remove_prefix(word_end + 1);
    }
    return false;
}
}  // namespace

http1_server_request_parse_state::http1_server_request_parse_state() noexcept
    : request_(http_request_access::make()) {}

http_request http1_server_request_parse_state::take_request() && noexcept {
    return std::exchange(request_, http_request_access::make());
}

http1_request_body_plan http1_server_request_parse_state::take_body_plan() && noexcept {
    return std::exchange(body_plan_, http1_request_body_plan(http_request_expectations{}));
}

http1_request_connection_plan http1_server_request_parse_state::take_connection_plan() && noexcept {
    return std::exchange(connection_plan_, http1_request_connection_plan::http11_close());
}

void http1_server_request_parse_state::parse_head(
    std::string_view buffer, std::size_t header_search_offset) {
    // Bytes before header_search_offset were classified by an earlier call on
    // this state; an earlier search offset discards that progress.
    auto scan = need_request_head() != nullptr ? *need_request_head()
                                               : http1_server_need_request_head{};
    scan.leading_bytes_ = std::min(scan.leading_bytes_, header_search_offset);
    scan.request_line_end_ = scan.request_line_end_ <= header_search_offset ? scan.request_line_end_ : 0;

    // Incomplete input allocates no descriptor storage. The scan state is
    // published first so an allocation failure while binding descriptors
    // below cannot leave an earlier head_ready result beside the new request.
    body_plan_ = http1_request_body_plan(http_request_expectations{});
    connection_plan_ = http1_request_connection_plan::http11_close();
    progress_ = scan;
    request_.reset();

    const auto fail = [this](http_parse_error error) noexcept {
        // A rejected head leaves the message boundary unknown and its content
        // unconsumed, so the connection cannot be reused (RFC 9112 sections 6.1
        // and 6.3). The request version may already have been accepted when a
        // later target/framing semantic check fails; preserve it so an HTTP/1.0
        // error response is not silently upgraded to HTTP/1.1.
        const auto connection_plan = connection_plan_.require_close();
        request_.reset();
        progress_ = http1_server_request_parse_failure(error);
        connection_plan_ = connection_plan;
    };

    const auto header_bytes = find_http_request_head_end(buffer, scan.leading_bytes_, header_search_offset);
    if (header_bytes == std::string_view::npos) {
        if (scan.request_line_end_ == 0) {
            if (const auto error = classify_http_request_line_prefix(
                    buffer, scan.leading_bytes_, header_search_offset, scan.request_line_end_)) {
                return fail(*error);
            }
        }
        // A field section line ending in a bare LF can never complete the
        // CRLF-delimited head (RFC 9112 section 2.2); answer it at once rather
        // than waiting for the header timeout.
        if (scan.request_line_end_ != 0 &&
            has_http_bare_line_feed(buffer, std::max(scan.request_line_end_, header_search_offset))) {
            return fail(http_parse_error::invalid_header);
        }
        if (buffer.size() >= max_http_header_bytes) {
            return fail(classify_head_cap(buffer, scan.leading_bytes_, scan.request_line_end_));
        }
        progress_ = scan;
        return;
    }

    parsed_request_header_block block;
    if (const auto error = parse_http_header_block(buffer, scan.leading_bytes_, header_bytes, block)) {
        return fail(*error);
    }

    // parse_http_header_block scans the method through the token table. Preserve the
    // exact wire token: method registration is extensible, while http_known_method is
    // only the framework's routing/response-semantics classification.
    const auto method = block.method_.bind(buffer);
    http_request_access::set_method(request_, method);
    const auto known_method = request_.known_method();

    const auto target = block.target_.bind(buffer);
    http_request_access::set_target(request_, target);

    const auto protocol_version = block.protocol_version_;
    http_request_access::set_protocol_version(request_, protocol_version);
    // Publish the version-specific request contract before any validation that
    // can fail after the version line itself has been accepted. The final
    // disposition is already derived from the parsed Connection fields and is
    // tightened to close by body/response policy later.
    connection_plan_ = protocol_version == http_protocol_version::http11
                           ? plan_http11_request_connection(block.connection_options_.close())
                           : plan_http10_request_connection(block.connection_options_.close(),
                                 block.connection_options_.keep_alive());
    if (known_method == http_known_method::connect) {
        connection_plan_ = connection_plan_.with_tunnel_request();
    }
    // RFC 9110 10.1.4 obliges the TE sender to list "TE" in Connection; it does
    // not oblige the recipient to reject. The server never acts on TE (it does
    // not condition trailers on it), so a bare TE is simply ignored.

    request_target_view target_view;
    if (!parse_request_target(known_method, target, target_view)) {
        return fail(http_parse_error::invalid_request_target);
    }
    apply_parsed_request_target(request_, target_view);
    if (protocol_version == http_protocol_version::http11 && block.host_header_index_ < 0) {
        return fail(http_parse_error::missing_host);
    }
    const auto host_header_index = block.host_header_index_;

    const auto content_length = block.content_length_.value();
    const auto& transfer_encoding = block.transfer_encoding_.value();
    if (transfer_encoding.has_value() && content_length.has_value()) {
        return fail(http_parse_error::invalid_transfer_encoding);
    }

    if (known_method == http_known_method::connect) {
        // RFC 9110 section 9.3.6: CONNECT content has no defined meaning and
        // bytes after the head belong to the tunnel, so any content framing
        // leaves the message boundary ambiguous. A zero Content-Length frames
        // no content (RFC 9112 section 6.3). TRACE content (section 9.3.8) is
        // a sender prohibition only; its framing is unambiguous, so it is
        // framed and consumed like any other content.
        if (transfer_encoding.has_value()) {
            return fail(http_parse_error::invalid_transfer_encoding);
        }
        if (content_length.has_value() && *content_length != 0) {
            return fail(http_parse_error::invalid_content_length);
        }
    }

    const auto* final_chunked =
        transfer_encoding.has_value() ? transfer_encoding->final_chunked() : nullptr;
    if (transfer_encoding.has_value() && final_chunked == nullptr) {
        return fail(http_parse_error::invalid_transfer_encoding);
    }

    // RFC 9112 section 6.1: Transfer-Encoding in an HTTP/1.0 request must be treated
    // as faulty framing; the error path closes the connection after replying.
    if (transfer_encoding.has_value() && protocol_version == http_protocol_version::http10) {
        return fail(http_parse_error::invalid_transfer_encoding);
    }
    if (block.transfer_encoding_.unsupported()) {
        return fail(http_parse_error::unsupported_transfer_encoding);
    }

    const auto headers = block.headers_.view();
    http_request_access::reserve_headers(request_, headers.size());
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header_value = headers[i];
        auto value = header_value.value_.bind(buffer);
        if (host_header_index >= 0 && i == static_cast<std::size_t>(host_header_index)) {
            value = bound_host_field_value(target_view, value);
        }
        if (!http_request_access::add_header(
                request_, http_header_view{header_value.name_.bind(buffer), value}, header_value.kind_)) {
            return fail(http_parse_error::header_too_large);
        }
    }

    auto expectations = block.expectations_;
    if (protocol_version == http_protocol_version::http10) {
        expectations.ignore_continue();
    }
    if (final_chunked != nullptr) {
        body_plan_ = http1_request_body_plan(final_chunked->transfer_codings(), expectations);
    } else if (content_length.has_value()) {
        if (*content_length > (std::numeric_limits<std::size_t>::max)()) {
            return fail(http_parse_error::invalid_content_length);
        }
        body_plan_ =
            http1_request_body_plan(static_cast<std::size_t>(*content_length), expectations);
    } else {
        body_plan_ = http1_request_body_plan(expectations);
    }
    progress_ = http1_server_request_head_ready(header_bytes);
}

http_request make_parsed_http_request(
    borrowed_text method, borrowed_text target, std::span<const http_header_view> headers,
    borrowed_span<std::byte> body) {
    const auto method_view = method.view();
    const auto target_text = target.view();
    if (!is_valid_http_method_token(method_view)) {
        throw http_parse_protocol_error(http_parse_error::invalid_request_line);
    }
    auto request = http_request_access::make();
    http_request_access::set_method(request, method_view);
    http_request_access::set_target(request, target_text);

    request_target_view target_view;
    if (!parse_request_target(request.known_method(), target_text, target_view)) {
        throw http_parse_protocol_error(http_parse_error::invalid_request_target);
    }
    // The same recipient field rules as the wire parsers: repeated singleton
    // fields such as Range or If-Range make the request invalid instead of
    // resolving to one of their lines.
    http_field_section_budget budget;
    detail::request_field_state fields(detail::request_field_framing::http1);
    static constexpr std::size_t inline_kind_capacity = 32;
    std::array<detail::http_field_kind, inline_kind_capacity> inline_kinds{};
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        if (!is_valid_http_header_name(header.name()) ||
            !is_valid_http_header_value(header.value())) {
            throw http_parse_protocol_error(http_parse_error::invalid_header);
        }
        if (!budget.add_field(header.name(), header.value())) {
            throw http_parse_protocol_error(http_parse_error::header_too_large);
        }
        const auto kind = detail::classify_http_field(header.name());
        if (i < inline_kind_capacity) {
            inline_kinds[i] = kind;
        }
        if (detail::validate_request_field(kind, header.value(), detail::http_field_list_role::recipient, fields) !=
            detail::request_field_status::ok) {
            throw http_parse_protocol_error(kind == detail::http_field_kind::host
                                                ? http_parse_error::invalid_host
                                                : http_parse_error::invalid_header);
        }
    }

    apply_parsed_request_target(request, target_view);
    http_request_access::reserve_headers(request, headers.size());
    for (std::size_t i = 0; i < headers.size(); ++i) {
        auto field = headers[i];
        const auto kind = i < inline_kind_capacity
                              ? inline_kinds[i]
                              : detail::classify_http_field(field.name());
        if (kind == detail::http_field_kind::host) {
            field = http_header_view{field.name(), bound_host_field_value(target_view, field.value())};
        }
        if (!http_request_access::add_header(request, field, kind)) {
            throw http_parse_protocol_error(http_parse_error::header_too_large);
        }
    }
    http_request_access::set_body(request, body.view());
    return request;
}

bool should_drop_invalid_cleartext_http1_input(
    std::string_view buffer, http1_server_request_parse_failure_source source_value) noexcept {
    if (source_value != http1_server_request_parse_failure_source::request_line) {
        return false;
    }

    buffer.remove_prefix(detail::http_request_leading_empty_line_bytes(buffer, 0));
    if (buffer.empty()) {
        return false;
    }
    // A first line that names an HTTP version shows the peer speaks HTTP and
    // earns the 400 of RFC 9112 sections 2.2 and 3, even when the line starts
    // with whitespace. Other input (TLS records, SSH banners) is dropped
    // silently; without a complete line, so is input whose first byte cannot
    // start a method.
    const auto line_end = buffer.find("\r\n");
    if (names_http_version(buffer.substr(0, line_end))) {
        return false;
    }
    return line_end != std::string_view::npos || !is_http_token_char(static_cast<unsigned char>(buffer.front()));
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] http_header_slice make_slice(std::size_t offset, std::size_t length) noexcept {
    return http_header_slice{
        .offset_ = static_cast<std::uint32_t>(offset), .length_ = static_cast<std::uint32_t>(length)};
}

// The request line: method SP request-target SP HTTP-version CRLF (RFC 9112
// section 3.1). On success `cursor` sits on the first header field and the
// block carries the accepted protocol version.
[[nodiscard]] std::optional<http_parse_error> parse_request_line(std::string_view buffer,
    std::size_t headers_end, std::size_t& cursor_value, parsed_request_header_block& block) noexcept {
    const auto method_start = cursor_value;
    while (cursor_value < headers_end && is_http_token_char(static_cast<unsigned char>(buffer[cursor_value]))) {
        ++cursor_value;
    }
    if (cursor_value == method_start || cursor_value >= headers_end || buffer[cursor_value] != ' ') {
        return http_parse_error::invalid_request_line;
    }
    block.method_ = make_slice(method_start, cursor_value - method_start);
    ++cursor_value;

    const auto target_start = cursor_value;
    while (cursor_value < headers_end && buffer[cursor_value] != ' ') {
        if (buffer[cursor_value] == '\r' || buffer[cursor_value] == '\n') {
            return http_parse_error::invalid_request_line;
        }
        ++cursor_value;
    }
    if (cursor_value == target_start || cursor_value >= headers_end) {
        return http_parse_error::invalid_request_line;
    }
    block.target_ = make_slice(target_start, cursor_value - target_start);
    ++cursor_value;

    const auto version_start = cursor_value;
    while (cursor_value < headers_end && buffer[cursor_value] != '\r') {
        if (buffer[cursor_value] == '\n' || buffer[cursor_value] == ' ') {
            return http_parse_error::invalid_request_line;
        }
        ++cursor_value;
    }
    if (cursor_value == version_start || cursor_value + 1 >= headers_end || buffer[cursor_value + 1] != '\n') {
        return http_parse_error::invalid_request_line;
    }
    const auto version = buffer.substr(version_start, cursor_value - version_start);
    cursor_value += 2;

    const auto parsed_version = parse_http1_version(version);
    if (parsed_version == http1_version_parse_result::invalid) {
        return http_parse_error::invalid_request_line;
    }
    if (parsed_version == http1_version_parse_result::unsupported) {
        return http_parse_error::unsupported_http_version;
    }
    block.protocol_version_ = parsed_version == http1_version_parse_result::http10
                                  ? http_protocol_version::http10
                                  : http_protocol_version::http11;
    return std::nullopt;
}

// What one request header means on HTTP/1: the shared semantic validator
// decides whether the value is acceptable, and the HTTP/1-specific fields are
// parsed into typed state on the block. Finding the bytes is the loop's job
// below; every rule about what they mean is here.
[[nodiscard]] std::optional<http_parse_error> apply_request_header(http_field_kind kind,
    std::string_view value, bool ignore_upgrade, parsed_request_header_block& block) {
    if (validate_request_field(kind, value, http_field_list_role::recipient, block.fields_) !=
        request_field_status::ok) {
        return kind == http_field_kind::host ? http_parse_error::invalid_host
                                             : http_parse_error::invalid_header;
    }
    switch (kind) {
        case http_field_kind::content_length:
            if (const auto error = parse_http1_content_length_field<http_parse_error>(value, block.content_length_)) {
                return error;
            }
            break;
        case http_field_kind::transfer_encoding:
            if (const auto error = parse_http1_transfer_encoding_field<http_parse_error>(value, block.transfer_encoding_)) {
                return error;
            }
            break;
        case http_field_kind::connection:
            if (block.connection_options_.parse_field(value, http_connection_field_role::request_recipient) !=
                http_field_list_parse_status::ok) {
                return http_parse_error::invalid_connection;
            }
            break;
        case http_field_kind::expect:
            block.expectations_.parse_field(value);
            break;
        case http_field_kind::upgrade:
            // RFC 9110 section 7.8 requires a server to ignore Upgrade in
            // an HTTP/1.0 request. The bytes still have to be a valid
            // generic field value, but Upgrade-specific grammar must not
            // turn an otherwise valid HTTP/1.0 request into a 400.
            if (!ignore_upgrade &&
                http_upgrade_protocols{}.parse_field(value, http_field_list_role::recipient) !=
                    http_field_list_parse_status::ok) {
                return http_parse_error::invalid_upgrade;
            }
            break;
        default:
            break;
    }

    return std::nullopt;
}

// The header section: one field line at a time, name and value delimited by
// ':' and CRLF with optional whitespace trimmed off the value.
[[nodiscard]] std::optional<http_parse_error> parse_header_fields(std::string_view buffer,
    std::size_t headers_end, std::size_t cursor_value, bool ignore_upgrade,
    parsed_request_header_block& block) {
    buffer = buffer.substr(0, headers_end);
    while (cursor_value < headers_end) {
        // The field-count rule of http_field_section_budget.
        if (block.headers_.size() == max_http_header_fields) {
            return http_parse_error::header_too_large;
        }
        const auto field = scan_http1_field_line(buffer, cursor_value);
        if (!field) {
            return http_parse_error::invalid_header;
        }
        const auto name = field->name();
        const auto value = field->value();

        const auto kind = classify_http_field(name);
        if (const auto error = apply_request_header(kind, value, ignore_upgrade, block)) {
            return error;
        }

        block.headers_.append(
            parsed_request_header_slot{.name_ = make_slice(static_cast<std::size_t>(name.data() - buffer.data()), name.size()),
                .value_ = make_slice(static_cast<std::size_t>(value.data() - buffer.data()), value.size()),
                .kind_ = kind});
        if (kind == http_field_kind::host) {
            block.host_header_index_ = static_cast<known_request_header_index_type>(block.headers_.size() - 1);
        }
    }

    return std::nullopt;
}

}  // namespace

std::size_t find_http_header_end(std::string_view buffer, std::size_t search_offset) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    if (buffer.size() < 4) {
        return std::string_view::npos;
    }

    auto cursor_value = std::max<std::size_t>(3, search_offset);
    while (cursor_value < buffer.size()) {
        const auto i =
            buffer[cursor_value] == '\n' ? cursor_value : buffer.find('\n', cursor_value + 1);
        if (i == std::string_view::npos) {
            return std::string_view::npos;
        }
        if (buffer[i - 1] == '\r') {
            if (buffer[i - 2] == '\n' && buffer[i - 3] == '\r') {
                return i + 1;
            }
            if (i + 2 < buffer.size() && buffer[i + 1] == '\r' && buffer[i + 2] == '\n') {
                return i + 3;
            }
            cursor_value = i + 3;
            continue;
        }
        cursor_value = i + 1;
    }

    return std::string_view::npos;
}

bool has_http_bare_line_feed(std::string_view buffer, std::size_t search_offset) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    for (auto line_feed = buffer.find('\n', std::max<std::size_t>(search_offset, 1));
        line_feed != std::string_view::npos; line_feed = buffer.find('\n', line_feed + 1)) {
        if (buffer[line_feed - 1] != '\r') {
            return true;
        }
    }
    return false;
}

std::size_t http_request_leading_empty_line_bytes(std::string_view buffer, std::size_t skipped) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    auto cursor_value = std::min(skipped, buffer.size());
    for (;;) {
        if (cursor_value < buffer.size() && buffer[cursor_value] == '\n') {
            ++cursor_value;
        } else if (buffer.substr(cursor_value).starts_with("\r\n")) {
            cursor_value += 2;
        } else {
            return cursor_value;
        }
    }
}

std::size_t find_http_request_head_end(
    std::string_view buffer, std::size_t& leading_bytes, std::size_t search_offset) noexcept {
    leading_bytes = http_request_leading_empty_line_bytes(buffer, leading_bytes);
    if (leading_bytes >= max_http_header_bytes) {
        return std::string_view::npos;
    }
    const auto head_end = find_http_header_end(
        buffer.substr(leading_bytes, max_http_header_bytes - leading_bytes),
        search_offset > leading_bytes ? search_offset - leading_bytes : 0);
    return head_end == std::string_view::npos ? head_end : leading_bytes + head_end;
}

std::optional<http_parse_error> classify_http_request_line_prefix(std::string_view buffer,
    std::size_t leading_bytes, std::size_t search_offset, std::size_t& request_line_end) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    if (leading_bytes >= buffer.size()) {
        return std::nullopt;
    }
    // The leading scan stops at a lone CR that may still become an empty line.
    if (buffer[leading_bytes] == '\r' && leading_bytes + 1 == buffer.size()) {
        return std::nullopt;
    }
    if (!is_http_token_char(static_cast<unsigned char>(buffer[leading_bytes]))) {
        return http_parse_error::invalid_request_line;
    }
    const auto line_feed = buffer.find('\n', std::max(leading_bytes, search_offset));
    if (line_feed == std::string_view::npos) {
        return std::nullopt;
    }
    parsed_request_header_block block;
    auto cursor_value = leading_bytes;
    if (const auto error = parse_request_line(buffer, line_feed + 1, cursor_value, block)) {
        return error;
    }
    request_line_end = cursor_value;
    return std::nullopt;
}

std::optional<http_parse_error> parse_http_header_block(std::string_view buffer,
    std::size_t leading_bytes, std::size_t header_bytes, parsed_request_header_block& block) {
    const auto headers_end = header_bytes - 2;
    auto cursor_value = leading_bytes;
    if (const auto error = parse_request_line(buffer, headers_end, cursor_value, block)) {
        return error;
    }
    return parse_header_fields(
        buffer, headers_end, cursor_value, block.protocol_version_ == http_protocol_version::http10, block);
}

}  // namespace ruvia::detail

namespace ruvia {

http_protocol_error http_parse_protocol_error(http_parse_error error) noexcept {
    switch (error) {
        case http_parse_error::header_too_large:
            return http_protocol_error(http_status::request_header_fields_too_large,
                "header_too_large", "request header is too large");
        case http_parse_error::uri_too_long:
            return http_protocol_error(
                http_status::uri_too_long, "uri_too_long", "request target is too long");
        case http_parse_error::invalid_request_line:
            return http_protocol_error(
                http_status::bad_request, "invalid_request_line", "invalid request line");
        case http_parse_error::unsupported_http_version:
            return http_protocol_error(http_status::http_version_not_supported,
                "unsupported_http_version", "unsupported HTTP version");
        case http_parse_error::invalid_request_target:
            return http_protocol_error(
                http_status::bad_request, "invalid_request_target", "invalid request target");
        case http_parse_error::invalid_header:
            return http_protocol_error(
                http_status::bad_request, "invalid_header", "invalid request header");
        case http_parse_error::invalid_connection:
            return http_protocol_error(
                http_status::bad_request, "invalid_connection", "invalid Connection header");
        case http_parse_error::invalid_upgrade:
            return http_protocol_error(
                http_status::bad_request, "invalid_upgrade", "invalid Upgrade header");
        case http_parse_error::missing_host:
            return http_protocol_error(
                http_status::bad_request, "missing_host", "missing Host header");
        case http_parse_error::invalid_host:
            return http_protocol_error(
                http_status::bad_request, "invalid_host", "invalid Host header");
        case http_parse_error::invalid_content_length:
            return http_protocol_error(http_status::bad_request, "invalid_content_length",
                "invalid Content-Length header");
        case http_parse_error::conflicting_content_length:
            return http_protocol_error(http_status::bad_request, "conflicting_content_length",
                "conflicting Content-Length header");
        case http_parse_error::invalid_transfer_encoding:
            return http_protocol_error(http_status::bad_request, "invalid_transfer_encoding",
                "invalid Transfer-Encoding header");
        case http_parse_error::unsupported_transfer_encoding:
            return http_protocol_error(http_status::not_implemented,
                "unsupported_transfer_encoding", "unsupported transfer encoding");
    }
    return http_protocol_error(http_status::bad_request, "invalid_request", "invalid HTTP request");
}

}  // namespace ruvia

namespace ruvia::detail {

namespace {

inline constexpr std::string_view chunked_transfer_encoding_header = "Transfer-Encoding: chunked\r\n";

inline constexpr std::string_view content_length_name = "Content-Length";

// Every field line was validated when it was inserted into the response
// (name, value, per-field grammar, forbidden fields, singletons). Only the
// rules that span several lines or depend on this head's framing remain.
[[nodiscard]] bool is_valid_http1_emitted_response_header(std::string_view name, std::string_view value,
    bool trailer_header_allowed, http_connection_options& connection_options,
    http_upgrade_protocols& upgrade_protocols) noexcept {
    const auto kind = classify_http_field(name);
    if (kind == http_field_kind::trailer) {
        return trailer_header_allowed || http_trim_ows(value).empty();
    }
    return parse_http_response_control_field(kind, value, connection_options, upgrade_protocols) ==
           http_response_control_field_status::ok;
}

void emit_response_head(const http_response& response, http_raw_wire_sink& sink_value, http_status_code response_status,
    std::string_view reason_phrase, std::string_view date_header, bool emit_chunked_transfer_encoding,
    std::string_view content_length_value) noexcept {
    emit_http1_status_line(sink_value, response_status, reason_phrase);

    for (const auto& header : response.headers()) {
        const auto known_bit = http_response_header_access::known_bit(header);
        // HTTP/1 framing belongs exclusively to http1_response_head_plan: the
        // handler's Content-Length and Transfer-Encoding (only ever the
        // replaceable chunked hint) yield to the plan's own framing fields.
        if (known_bit == response_header_transfer_encoding ||
            known_bit == response_header_content_length) {
            continue;
        }
        emit_http1_field_line(sink_value, header.name(), header.value());
    }

    const auto known_bits = response_known_header_bits(response);
    if ((known_bits & response_header_date) == 0 && !date_header.empty()) {
        sink_value.append(date_header);
    }
    if (emit_chunked_transfer_encoding) {
        sink_value.append(chunked_transfer_encoding_header);
    }
    if (!content_length_value.empty()) {
        sink_value.append(content_length_name);
        sink_value.append(std::string_view(": "));
        sink_value.append(content_length_value);
        sink_value.append(std::string_view("\r\n"));
    }
    sink_value.append(std::string_view("\r\n"));
}

template <typename sink_type>
void emit_response_trailer_fields(sink_type& sink, std::span<const http_header_view> fields) {
    for (const auto& field : fields) {
        emit_http1_field_line(sink, field.name(), field.value());
    }
}

// Consume aliased names, values and descriptors before append can reallocate.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void append_response_trailer_snapshot(std::string& output,
    std::span<const http_header_view> fields, std::size_t bytes) {
    std::array<char, max_http_header_bytes> scratch;
    http_raw_wire_sink sink(scratch.data());
    emit_response_trailer_fields(sink, fields);
    check_http_wire_end(sink.position(), scratch.data() + bytes);
    output.append(scratch.data(), bytes);
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

void append_http1_response_head(
    const http_response& response, http_response_head_buffer& head, const http1_response_head_plan& plan) {
    const auto body_plan = plan.body_plan();
    const auto* buffered = plan.buffered();
    const auto content_length_plan = buffered != nullptr
                                         ? detail::http_framed_buffered_response_head_plan(body_plan, buffered->content_length(), response)
                                         : detail::http_framed_streaming_response_head_plan(body_plan, response);
    const auto* framed = std::get_if<detail::http_framed_response_head_plan>(&content_length_plan);
    if (framed == nullptr) {
        switch (std::get<detail::http_framed_response_head_plan_error>(content_length_plan)) {
            case detail::http_framed_response_head_plan_error::invalid_content_length:
                throw http1_response_head_error("invalid explicit HTTP response Content-Length");
            case detail::http_framed_response_head_plan_error::response_status_mismatch:
                throw std::invalid_argument("HTTP/1 response plan status does not match response");
            case detail::http_framed_response_head_plan_error::response_representation_mismatch:
                throw std::invalid_argument("HTTP/1 response plan representation does not match response");
            case detail::http_framed_response_head_plan_error::connect_tunnel_required:
                throw http1_response_head_error("HTTP CONNECT tunnel response required");
        }
    }
    if (!detail::response_transfer_encoding_replaceable(response)) {
        throw http1_response_head_error("HTTP response Transfer-Encoding must be exactly chunked");
    }
    const auto response_status = body_plan.response_status();
    const bool chunked_payload_plan = plan.chunked_stream() != nullptr &&
                                      body_plan.transfer_encoding_allowed() && !body_plan.body_suppressed();
    if (chunked_payload_plan && plan.protocol_version() == http_protocol_version::http10) {
        throw http1_response_head_error("HTTP/1.0 response cannot use chunked Transfer-Encoding");
    }
    const bool emit_chunked_transfer_encoding = chunked_payload_plan;
    // HTTP/1 alone can frame a stream with chunked coding or connection close.
    // Those delimiters replace Content-Length except suppressed HEAD metadata.
    const auto content_length = emit_chunked_transfer_encoding ||
                                        (plan.close_delimited_stream() != nullptr && !body_plan.body_suppressed())
                                    ? std::nullopt
                                    : framed->content_length();
    const auto known_bits = detail::response_known_header_bits(response);

    const auto reason_phrase = http_reason_phrase(response_status);
    const auto date_header = (known_bits & detail::response_header_date) == 0
                                 ? detail::cached_date_header()
                                 : std::string_view{};
    // Reuse the formatted token for exact measurement and emission instead of
    // separately dividing the length to count its digits.
    // Buffered HEAD metadata retains the selected representation length.
    // A status-level no-content policy that still owns framing (205) is
    // canonicalized to zero for both buffered and streaming heads.
    std::array<char, http_max_unsigned_decimal_size> content_length_buffer;
    const auto content_length_value =
        content_length.has_value()
            ? http_format_unsigned(content_length_buffer, *content_length)
            : std::string_view{};

    // Measure the exact emitted head before touching reusable output storage.
    // This both bounds the unchecked raw stack sink and enforces the shared
    // field-section rule used by request parsing.
    http_field_section_budget head_budget;
    const auto charge = [](bool fits) {
        if (!fits) {
            throw http1_response_head_error("HTTP response head is too large");
        }
    };
    charge(head_budget.add(detail::http1_status_line_size(reason_phrase)));
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    for (const auto& header : response.headers()) {
        const auto known_bit = detail::http_response_header_access::known_bit(header);
        if (known_bit == detail::response_header_transfer_encoding ||
            known_bit == detail::response_header_content_length) {
            continue;
        }
        if (!detail::is_valid_http1_emitted_response_header(header.name(), header.value(),
                emit_chunked_transfer_encoding, connection_options, upgrade_protocols)) {
            throw http1_response_head_error("invalid HTTP response header");
        }
        charge(head_budget.add_field(header.name(), header.value()));
    }
    if (!detail::http_response_upgrade_pair_valid(connection_options, upgrade_protocols)) {
        throw http1_response_head_error("HTTP Upgrade response requires Connection: Upgrade");
    }
    if ((known_bits & detail::response_header_date) == 0 && !date_header.empty()) {
        charge(head_budget.add(date_header.size()));
    }
    if (emit_chunked_transfer_encoding) {
        charge(head_budget.add(detail::chunked_transfer_encoding_header.size()));
    }
    if (!content_length_value.empty()) {
        charge(head_budget.add_field(detail::content_length_name.size(), content_length_value.size()));
    }
    charge(head_budget.add(2));

    head.append_generated(head_budget.bytes(), [&](char* cursor_value) noexcept {
        detail::http_raw_wire_sink sink_value(cursor_value);
        detail::emit_response_head(response, sink_value, response_status, reason_phrase, date_header,
            emit_chunked_transfer_encoding, content_length_value);
        detail::check_http_wire_end(sink_value.position(), cursor_value + head_budget.bytes());
    });
}

void append_http1_response_trailers(
    std::string& output, const http_response_trailer_section& trailers) {
    const auto fields = trailers.fields();
    if (fields.empty()) {
        return;
    }
    auto budget = http_trailer_section_budget();
    for (const auto& field : fields) {
        if (!budget.add_field(field.name(), field.value())) {
            throw std::length_error("HTTP response trailers are too large");
        }
    }
    const auto bytes = budget.bytes() - std::string_view("\r\n").size();
    if (bytes > output.max_size() - output.size()) {
        throw std::length_error("HTTP response trailers are too large");
    }
    if (detail::http_wire_inputs_overlap(output, fields)) {
        detail::append_response_trailer_snapshot(output, fields, bytes);
        return;
    }
    output.reserve(output.size() + bytes);
    detail::http_string_wire_sink sink(output);
    detail::emit_response_trailer_fields(sink, fields);
}

namespace {

// Lists one generated option; an application Connection line is extended
// rather than replaced, so its own options survive.
void add_http1_connection_option(http_response& response, std::string_view option, bool field_present) {
    if (field_present) {
        response.header(detail::http_connection_field_name, option,
            http_response_header_options{.mode_ = http_response_header_mode::append});
    } else {
        response.header_stable_view(detail::http_connection_field_name, option);
    }
}

// Visits each protocol the Upgrade fields of `request` offer, in field order,
// until `visitor` returns false. Malformed members end the visit.
template <typename visitor_type>
void visit_offered_upgrade_protocols(const http_request& request, visitor_type&& visitor) noexcept {
    const auto headers = request.headers();
    bool visiting = true;
    for (std::size_t index = 0; visiting && index < headers.size(); ++index) {
        if (detail::http_request_access::header_kind(request, index) != detail::http_field_kind::upgrade) {
            continue;
        }
        (void)detail::http_upgrade_protocols{}.parse_field(headers[index].value(), detail::http_field_list_role::recipient,
            [&visiting, &visitor](const detail::http_upgrade_protocol& protocol) noexcept {
                visiting = visitor(protocol);
                return visiting;
            });
    }
}

[[nodiscard]] bool request_offers_upgrade_protocol(
    const http_request& request, const detail::http_upgrade_protocol& selected) noexcept {
    bool offered = false;
    visit_offered_upgrade_protocols(request, [&offered, &selected](const detail::http_upgrade_protocol& protocol) noexcept {
        offered = detail::http_upgrade_protocol_equals(protocol, selected);
        return !offered;
    });
    return offered;
}

}  // namespace

http1_request_connection_plan http1_commit_final_response(
    http_response& response, http1_request_connection_plan plan) {
    const auto http1_control = detail::http1_final_response_control_plan(response);
    const auto response_options = http1_control.connection_options();
    const auto upgrade_protocols = http1_control.upgrade_protocols();
    const bool preserve_upgrade = upgrade_protocols.has_field();
    // A failed CONNECT never becomes a tunnel. RFC 9931 section 8 requires
    // closing instead of interpreting optimistic tunnel data as more requests.
    if (response_options.close() || (plan.tunnel_request() && !response.status().is_successful())) {
        plan = plan.require_close();
    }
    if (plan.disposition() == http1_close_policy::close_after_response) {
        http1_mark_connection_close(response, preserve_upgrade
                                                  ? http1_connection_close_field_policy::preserve_upgrade
                                                  : http1_connection_close_field_policy::close_only);
    } else {
        const bool generate_upgrade_option = preserve_upgrade && !response_options.upgrade();
        if (generate_upgrade_option) {
            add_http1_connection_option(
                response, detail::http_connection_upgrade_option, response_options.has_field());
        }
        if (plan.protocol_version() == http_protocol_version::http10 && !response_options.keep_alive()) {
            add_http1_connection_option(response, detail::http_connection_keep_alive_option,
                response_options.has_field() || generate_upgrade_option);
        }
    }
    return plan;
}

prepared_http1_response_stream prepare_http1_response_stream_head(
    http_response response, const http1_response_stream_plan& plan,
    http_response_trailer_intent trailer_intent) {
    auto commit_plan = plan_http_response_stream_commit(
        plan.framing(), plan.request_method(), response.status(), trailer_intent);
    const auto body_plan = commit_plan.body_plan();
    // HTTP/1.0 cannot delimit an open-ended response stream without closing the
    // connection, but a response whose method/status forbids payload is already
    // self-delimited. Make this decision at head commit, when the response status is
    // finally known, instead of pessimistically baking close into the pre-commit plan.
    const auto planned_connection =
        plan.request_connection_plan().disposition() == http1_close_policy::allow_reuse &&
                plan.close_policy() == http1_close_policy::allow_reuse &&
                (plan.framing() != http_response_stream_framing::http1_close_delimited ||
                    body_plan.body_suppressed())
            ? plan.request_connection_plan()
            : plan.request_connection_plan().require_close();
    const auto connection_plan = http1_commit_final_response(response, planned_connection);
    auto head = prepare_http_response_stream_head(std::move(response), std::move(commit_plan));
    const auto response_head_plan =
        plan.framing() == http_response_stream_framing::http1_chunked
            ? http1_chunked_response_stream_head_plan(head.commit_plan().body_plan(), connection_plan)
            : http1_close_delimited_response_stream_head_plan(
                  head.commit_plan().body_plan(), connection_plan);
    return prepared_http1_response_stream(std::move(head), response_head_plan, connection_plan);
}

bool http1_request_offers_upgrade(const http_request& request) noexcept {
    if (request.protocol_version() != http_protocol_version::http11 ||
        request.known_method() == http_known_method::connect) {
        return false;
    }
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    const auto headers = request.headers();
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const auto kind = detail::http_request_access::header_kind(request, index);
        if (kind == detail::http_field_kind::connection) {
            if (connection_options.parse_field(headers[index].value(),
                    detail::http_connection_field_role::request_recipient) != detail::http_field_list_parse_status::ok) {
                return false;
            }
        } else if (kind == detail::http_field_kind::upgrade) {
            if (upgrade_protocols.parse_field(headers[index].value(), detail::http_field_list_role::recipient) !=
                detail::http_field_list_parse_status::ok) {
                return false;
            }
        }
    }
    return connection_options.upgrade() && upgrade_protocols.has_protocol();
}

http1_response_head_plan prepare_http1_upgrade_response_head(http_response& response, const http_request& request) {
    if (!http1_request_offers_upgrade(request)) {
        throw http1_response_head_error("HTTP/1 request offers no protocol upgrade");
    }
    if (response.file_body() || response.has_multipart_file_body() || !response.body_bytes().empty() ||
        detail::response_has_known_header(
            response, detail::response_header_content_length | detail::response_header_transfer_encoding)) {
        throw http1_response_head_error("HTTP/1 Switching Protocols response cannot carry content or framing fields");
    }
    const auto http1_control = detail::http1_final_response_control_plan(response);
    // RFC 9110 §7.8: the server switches only to protocols the client offered.
    if (http1_control.upgrade_protocols().has_field()) {
        bool offered = http1_control.upgrade_protocols().has_protocol();
        for (const auto& header : response.headers()) {
            if (offered && detail::http_response_header_access::known_bit(header) == detail::response_header_upgrade) {
                (void)detail::http_upgrade_protocols{}.parse_field(header.value(), detail::http_field_list_role::sender,
                    [&offered, &request](const detail::http_upgrade_protocol& selected) noexcept {
                        offered = request_offers_upgrade_protocol(request, selected);
                        return offered;
                    });
            }
        }
        if (!offered) {
            throw http1_response_head_error("HTTP/1 Switching Protocols response names a protocol the request did not offer");
        }
    } else {
        std::string_view offered_protocol;
        visit_offered_upgrade_protocols(request, [&offered_protocol](const detail::http_upgrade_protocol& protocol) noexcept {
            const auto end = protocol.version_.empty() ? protocol.name_.data() + protocol.name_.size()
                                                       : protocol.version_.data() + protocol.version_.size();
            offered_protocol = std::string_view(protocol.name_.data(), static_cast<std::size_t>(end - protocol.name_.data()));
            return false;
        });
        response.header("Upgrade", offered_protocol);
    }
    if (!http1_control.connection_options().upgrade()) {
        add_http1_connection_option(response, detail::http_connection_upgrade_option,
            http1_control.connection_options().has_field());
    }
    response.status_code_ = http_status::switching_protocols;
    return http1_close_delimited_response_stream_head_plan(
        plan_http_response_body(http_known_method::get, http_status::switching_protocols),
        http1_request_connection_plan::http11_close());
}

}  // namespace ruvia

namespace ruvia {

http_response_head_buffer::http_response_head_buffer(const http_response_head_buffer& other)
    : heap_(other.heap_) {
}

http_response_head_buffer::http_response_head_buffer(http_response_head_buffer&& other) noexcept
    : heap_(std::move(other.heap_)) {
    copy_committed_state(other);
}

http_response_head_buffer& http_response_head_buffer::operator=(const http_response_head_buffer& other) {
    if (this != &other) {
        if (other.heap_.size() > heap_.capacity()) {
            // Retire the old allocation only after the complete state is published.
            std::string prepared(other.heap_);
            heap_.swap(prepared);
            copy_committed_state(other);
            return *this;
        }
        heap_ = other.heap_;
        copy_committed_state(other);
    }
    return *this;
}

http_response_head_buffer& http_response_head_buffer::operator=(
    http_response_head_buffer&& other) noexcept {
    if (this != &other) {
        heap_ = std::move(other.heap_);
        copy_committed_state(other);
    }
    return *this;
}

void http_response_head_buffer::copy_committed_state(const http_response_head_buffer& other) noexcept {
    // Heap replacement must succeed before publishing the inline bytes and state.
    if (const auto* const stack_state = std::get_if<stack_state_type>(&other.state_)) {
        std::memcpy(stack_.data(), other.stack_.data(), stack_state->used_);
    }
    state_ = other.state_;
}

void http_response_head_buffer::reset() noexcept {
    // Publish the empty inline state before retiring storage can reenter this buffer.
    state_.emplace<stack_state_type>();
    http_clear_string_retaining_small(heap_, response_head_retained_heap_bytes);
}

void http_response_head_buffer::spill_to_heap(std::size_t min_capacity) {
    heap_.reserve(std::max(stack_.size() * 2, min_capacity));
    heap_.assign(stack_.data(), std::get<stack_state_type>(state_).used_);
    state_.emplace<heap_state_type>();
}

void http_response_head_buffer::append(std::string_view value) {
    if (value.empty()) {
        return;
    }
    if (auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        if (value.size() <= stack_.size() - stack_state->used_) {
            std::memmove(stack_.data() + stack_state->used_, value.data(), value.size());
            stack_state->used_ += value.size();
            return;
        }
        if (value.size() > heap_.max_size() - stack_state->used_) {
            throw std::length_error("HTTP response head is too large");
        }
        spill_to_heap(stack_state->used_ + value.size());
        heap_.append(value);
        return;
    }
    if (value.size() > heap_.max_size() - heap_.size()) {
        throw std::length_error("HTTP response head is too large");
    }
    if (detail::http_text_aliases_string(value, heap_)) {
        std::string copy(value);
        heap_.append(copy);
        return;
    }
    heap_.append(value);
}

void http_response_head_buffer::append(char value) {
    if (auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        if (stack_state->used_ < stack_.size()) {
            stack_[stack_state->used_++] = value;
            return;
        }
        spill_to_heap(stack_state->used_ + 1);
    }
    heap_.push_back(value);
}

void http_response_head_buffer::append_unsigned(std::uint64_t value) {
    std::array<char, http_max_unsigned_decimal_size> digits;
    append(http_format_unsigned(digits, value));
}

void http_response_head_buffer::reserve_additional(std::size_t size) {
    if (std::holds_alternative<heap_state_type>(state_)) {
        if (size > heap_.max_size() - heap_.size()) {
            throw std::length_error("HTTP response head is too large");
        }
        heap_.reserve(heap_.size() + size);
        return;
    }
    const auto used = std::get<stack_state_type>(state_).used_;
    if (size <= stack_.size() - used) {
        return;
    }
    if (size > heap_.max_size() - used) {
        throw std::length_error("HTTP response head is too large");
    }
    spill_to_heap(used + size);
}

std::string_view http_response_head_buffer::view() const& noexcept {
    if (const auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        return std::string_view(stack_.data(), stack_state->used_);
    }
    return std::string_view(heap_);
}

bool http_response_head_buffer::can_append_on_stack(std::size_t size) const noexcept {
    const auto* const stack_state = std::get_if<stack_state_type>(&state_);
    return stack_state != nullptr && size <= stack_.size() - stack_state->used_;
}

}  // namespace ruvia

namespace ruvia {

namespace {

struct http_chunk_size_line final {
    std::size_t size_{0};
    // Line bytes beyond the shortest hex spelling of size_: leading zeros and
    // chunk-ext. A decoder charges them to a per-message framing budget.
    std::size_t overhead_bytes_{0};
};

bool is_valid_http_chunk_extension(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }

    std::size_t cursor_value = 0;
    const auto skip_bws = [&value, &cursor_value]() noexcept {
        while (cursor_value < value.size() && is_http_ows_char(value[cursor_value])) {
            ++cursor_value;
        }
    };
    const auto parse_token = [&value, &cursor_value]() noexcept {
        const auto begin = cursor_value;
        while (
            cursor_value < value.size() && is_http_token_char(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        return cursor_value != begin;
    };

    while (cursor_value < value.size()) {
        skip_bws();
        if (cursor_value == value.size()) {
            // Reached end-of-line after consuming whitespace with no ";": the
            // chunk-ext grammar (RFC 9112 7.1) permits BWS only before ";"/"=",
            // never as trailing space before CRLF. Accepting "5 " is a
            // request-smuggling differential of the same class as the leading-OWS
            // case that parse_http_chunk_size_line already rejects.
            return false;
        }
        if (value[cursor_value] != ';') {
            return false;
        }
        ++cursor_value;
        skip_bws();
        if (!parse_token()) {
            return false;
        }
        const auto after_name = cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            // Bare ext-name at end-of-line: valid only with no trailing BWS
            // between the name and the CRLF (RFC 9112 7.1).
            return cursor_value == after_name;
        }
        if (value[cursor_value] == ';') {
            continue;
        }
        if (value[cursor_value] != '=') {
            return false;
        }
        ++cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            return false;
        }
        if (value[cursor_value] == '"') {
            const auto quoted_size = http_quoted_string_size(value.substr(cursor_value));
            if (quoted_size == 0) {
                return false;
            }
            cursor_value += quoted_size;
        } else if (!parse_token()) {
            return false;
        }
        const auto before_trailing_bws = cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            // End-of-line after a chunk-ext value is valid only if it lands
            // exactly on the value; whitespace between the value and CRLF is
            // rejected for the same reason as trailing space after the size.
            return cursor_value == before_trailing_bws;
        }
        if (value[cursor_value] != ';') {
            return false;
        }
    }
    return true;
}

// Parses `chunk-size [ chunk-ext ]` (RFC 9112 7.1) without its CRLF; nullopt on
// any malformed or overflowing line.
std::optional<http_chunk_size_line> parse_http_chunk_size_line(std::string_view value) noexcept {
    // RFC 9112: chunk-size starts at the first byte of the line. Leading OWS
    // before the size is a known request-smuggling vector and is rejected,
    // matching picohttpparser/llhttp strict parsing.
    std::size_t cursor_value = 0;
    std::size_t leading_zero_digits = 0;
    std::size_t parsed_value = 0;
    constexpr auto max_before_shift = std::numeric_limits<std::size_t>::max() >> 4U;
    while (cursor_value < value.size()) {
        const int nibble = http_decode_hex_nibble(value[cursor_value]);
        if (nibble < 0) {
            break;
        }
        if (parsed_value > max_before_shift) {
            return std::nullopt;
        }
        if (parsed_value == 0 && nibble == 0) {
            ++leading_zero_digits;
        }
        parsed_value = (parsed_value << 4U) | static_cast<std::size_t>(nibble);
        ++cursor_value;
    }
    if (cursor_value == 0 || !is_valid_http_chunk_extension(value.substr(cursor_value))) {
        return std::nullopt;
    }

    // The shortest spelling of a size drops its leading zeros but keeps one
    // digit for a zero size.
    const auto significant_digits = std::max<std::size_t>(cursor_value - leading_zero_digits, 1);
    return http_chunk_size_line{.size_ = parsed_value, .overhead_bytes_ = value.size() - significant_digits};
}

// The HTTP/1 received trailer-block field parser. `block` holds the field
// lines between the last chunk and the empty line that ends the trailer
// section. Values are exposed with field-line OWS trimmed.
// `obs_fold` decides whether a line starting with OWS continues the previous
// field (RFC 9112 section 5.2). Rejected, such a line is malformed. Accepted,
// the field is validated and charged as its HTTP unfolding (each obs-fold and
// adjacent OWS is one SP), and the visitor receives the received value span
// with its folds in place. Every field is charged to the section budget;
// syntax errors and the budget fail the whole section. The visitor receives
// views borrowing `block`.
template <typename visitor_type>
[[nodiscard]] bool visit_http1_trailer_block(
    std::string_view block, http1_trailer_obs_fold obs_fold, visitor_type&& visitor) {
    auto budget = http_trailer_section_budget();
    const auto line_at = [block](std::size_t begin, std::size_t end) noexcept {
        return end == std::string_view::npos ? block.substr(begin) : block.substr(begin, end - begin);
    };
    std::size_t cursor_value = 0;
    while (cursor_value < block.size()) {
        auto line_end = block.find("\r\n", cursor_value);
        const auto line = line_at(cursor_value, line_end);
        // Finding the separator also proves that the complete name is a token.
        const auto colon = http_token_prefix_size(line);
        if (colon == 0 || colon == line.size() || line[colon] != ':') {
            return false;
        }
        const auto name = line.substr(0, colon);
        // The unfolded value spans the first to the last non-empty piece, each
        // fold between them counting as one SP.
        auto piece = http_trim_ows(line.substr(colon + 1));
        auto value = piece;
        std::size_t value_bytes = 0;
        std::size_t pending_separators = 0;
        for (;;) {
            if (!is_valid_http_field_value_bytes(piece)) {
                return false;
            }
            if (!piece.empty()) {
                if (value_bytes == 0) {
                    value = piece;
                } else {
                    value_bytes += pending_separators;
                    value = std::string_view(
                        value.data(), static_cast<std::size_t>(piece.data() - value.data()) + piece.size());
                }
                value_bytes += piece.size();
                pending_separators = 0;
            }
            if (obs_fold == http1_trailer_obs_fold::reject || line_end == std::string_view::npos ||
                line_end + 2 >= block.size() || !is_http_ows_char(block[line_end + 2])) {
                break;
            }
            ++pending_separators;
            const auto piece_begin = line_end + 2;
            line_end = block.find("\r\n", piece_begin);
            piece = http_trim_ows(line_at(piece_begin, line_end));
        }
        if (!budget.add_field(name.size(), value_bytes)) {
            return false;
        }
        visitor(name, value);
        if (line_end == std::string_view::npos) {
            break;
        }
        cursor_value = line_end + 2;
    }
    return true;
}

}  // namespace

http1_chunk_decode_result http1_chunk_decode_result::make_need_more(std::size_t consumed_bytes) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_need_more(consumed_bytes));
}

http1_chunk_decode_result http1_chunk_decode_result::make_body_chunk(
    std::size_t consumed_bytes, std::string_view bytes_value) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_body_chunk_view(consumed_bytes, bytes_value));
}

http1_chunk_decode_result http1_chunk_decode_result::make_complete(
    std::size_t consumed_bytes, std::span<const http_header> trailers) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_complete_view(consumed_bytes, trailers));
}

http1_chunk_decode_result http1_chunk_decode_result::make_failure(
    std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_failure(consumed_bytes, error));
}

http1_chunked_body_decoder::http1_chunked_body_decoder(http1_chunked_body_decoder_config config)
    : body_limit_(config.body_limit_),
      obs_fold_(config.obs_fold_),
      trailer_message_(config.trailer_message_) {}

http1_chunked_body_decoder::~http1_chunked_body_decoder() = default;
http1_chunked_body_decoder::http1_chunked_body_decoder(http1_chunked_body_decoder&&) noexcept = default;
http1_chunked_body_decoder& http1_chunked_body_decoder::operator=(http1_chunked_body_decoder&&) noexcept = default;

http1_chunk_decode_result http1_chunked_body_decoder::decode(std::string_view available) {
    return decode(available, std::numeric_limits<std::size_t>::max());
}

http1_chunk_decode_result http1_chunked_body_decoder::decode(
    std::string_view available, std::size_t max_body_bytes) {
    if (max_body_bytes == 0) {
        throw std::invalid_argument("HTTP/1 chunk decode body quota must be greater than zero");
    }
    if (const auto* error = std::get_if<http1_chunk_decode_error>(&state_)) {
        return http1_chunk_decode_result::make_failure(0, *error);
    }
    // Published trailers live until this next call.
    trailers_.clear();

    std::size_t cursor = 0;
    for (;;) {
        switch (std::get<progress>(state_)) {
            case progress::size_line: {
                // Every LF must end a CRLF (RFC 9112 section 7.1), so a bare LF is
                // rejected as soon as it arrives instead of waiting for a CRLF.
                const auto line = available.substr(cursor, max_http_header_bytes);
                const auto line_feed = line.find('\n', size_line_search_offset_);
                if (line_feed == std::string_view::npos) {
                    const auto line_bytes = line.size();
                    if (line_bytes >= max_http_header_bytes) {
                        return fail(cursor, http1_chunk_decode_error::framing_limit_exceeded);
                    }
                    size_line_search_offset_ = line_bytes;
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                if (line_feed == 0 || line[line_feed - 1] != '\r') {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                size_line_search_offset_ = 0;
                const auto line_end = line_feed - 1;
                const auto size_line = parse_http_chunk_size_line(line.substr(0, line_end));
                if (!size_line) {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                // RFC 9112 section 7.1.1: a recipient ought to limit the total
                // chunk-ext length of a message; leading zeros cost the same scan.
                if (max_http_header_bytes - framing_overhead_bytes_ < size_line->overhead_bytes_) {
                    return fail(cursor, http1_chunk_decode_error::framing_limit_exceeded);
                }
                framing_overhead_bytes_ += size_line->overhead_bytes_;
                cursor += line_end + 2;
                if (size_line->size_ == 0) {
                    state_ = progress::trailers;
                    trailer_search_offset_ = 0;
                } else {
                    if (body_limit_.addition_exceeds(decoded_bytes_, size_line->size_)) {
                        return fail(cursor, http1_chunk_decode_error::body_limit_exceeded);
                    }
                    decoded_bytes_ += size_line->size_;
                    remaining_ = size_line->size_;
                    state_ = progress::body;
                }
                break;
            }
            case progress::body: {
                const auto bytes_value = std::min({remaining_, available.size() - cursor, max_body_bytes});
                if (bytes_value == 0) {
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                const auto body = available.substr(cursor, bytes_value);
                remaining_ -= bytes_value;
                cursor += bytes_value;
                if (remaining_ == 0) {
                    state_ = progress::delimiter;
                    // A buffered delimiter is consumed with the final data bytes,
                    // so consumed_bytes covers the whole chunk.
                    if (available.size() - cursor >= 2 && !consume_delimiter(available, cursor)) {
                        return fail(cursor, http1_chunk_decode_error::invalid_framing);
                    }
                }
                return http1_chunk_decode_result::make_body_chunk(cursor, body);
            }
            case progress::delimiter:
                if (available.size() - cursor < 2) {
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                if (!consume_delimiter(available, cursor)) {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                break;
            case progress::trailers: {
                const auto trailers = available.substr(cursor, max_http_header_bytes);
                if (trailers.starts_with("\r\n")) {
                    state_ = progress::complete;
                    return http1_chunk_decode_result::make_complete(cursor + 2);
                }
                // Scan only bytes not examined by an earlier call. Each LF must be
                // preceded by CR; an LF directly after a CRLF ends the section.
                auto trailer_end = std::string_view::npos;
                for (auto line_feed = trailers.find('\n', trailer_search_offset_);
                    line_feed != std::string_view::npos; line_feed = trailers.find('\n', line_feed + 1)) {
                    if (line_feed == 0 || trailers[line_feed - 1] != '\r') {
                        return fail(cursor, http1_chunk_decode_error::invalid_framing);
                    }
                    if (line_feed >= 3 && trailers[line_feed - 2] == '\n') {
                        trailer_end = line_feed - 3;
                        break;
                    }
                }
                if (trailer_end == std::string_view::npos) {
                    if (trailers.size() >= max_http_header_bytes) {
                        return fail(cursor, http1_chunk_decode_error::framing_limit_exceeded);
                    }
                    trailer_search_offset_ = trailers.size();
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                const auto trailer_bytes = trailer_end + 4;
                if (!publish_trailers(trailers.substr(0, trailer_end))) {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                state_ = progress::complete;
                return http1_chunk_decode_result::make_complete(cursor + trailer_bytes, trailers_);
            }
            case progress::complete:
                return http1_chunk_decode_result::make_complete(cursor);
        }
    }
}

// Requires both delimiter bytes to be buffered at cursor.
bool http1_chunked_body_decoder::consume_delimiter(std::string_view available, std::size_t& cursor) noexcept {
    if (!available.substr(cursor).starts_with("\r\n")) {
        return false;
    }
    cursor += 2;
    state_ = progress::size_line;
    return true;
}

bool http1_chunked_body_decoder::publish_trailers(std::string_view trailers) {
    // Received fields that were not permitted as trailers are discarded, never
    // merged (RFC 9112 section 7.1.2), so framing validity depends only on
    // field syntax and the section budget.
    const auto discarded = [message = trailer_message_](std::string_view name) noexcept {
        return message == http1_trailer_message::request ? detail::is_forbidden_http_request_trailer_name(name)
                                                         : detail::is_forbidden_response_trailer_name(name);
    };
    const auto publish = [this, &discarded](std::string_view name, std::string_view value) {
        if (discarded(name)) {
            return;
        }
        // RFC 9112 section 5.2: each accepted obs-fold becomes SP before the
        // value is interpreted.
        if (const auto fold = detail::find_field_line_fold(value); fold != std::string_view::npos) {
            std::string unfolded;
            detail::copy_unfolded_field_lines(value, fold, detail::field_line_folding_policy::http, unfolded);
            trailers_.push_back(http_header::copy_with_lowercase_name(name, unfolded));
            return;
        }
        trailers_.push_back(http_header::copy_with_lowercase_name(name, value));
    };
    bool valid = false;
    try {
        valid = visit_http1_trailer_block(trailers, obs_fold_, publish);
    } catch (...) {
        trailers_.clear();
        throw;
    }
    if (!valid) {
        trailers_.clear();
    }
    return valid;
}

http1_chunk_decode_result http1_chunked_body_decoder::fail(
    std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept {
    state_ = error;
    return http1_chunk_decode_result::make_failure(consumed_bytes, error);
}

}  // namespace ruvia

namespace ruvia {

namespace detail {

// One zlib stage is an implementation detail of the coding sequence, not a
// second public decoder. Its address must stay fixed for the pinned zlib stream.
class transfer_coding_decoder final {
public:
    transfer_coding_decoder(http_transfer_coding coding, protocol_byte_limit decoded_limit);
    transfer_coding_decoder(const transfer_coding_decoder&) = delete;
    transfer_coding_decoder& operator=(const transfer_coding_decoder&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] http_transfer_coding_decode_result finish_input() noexcept;
    [[nodiscard]] bool received_input() const noexcept {
        return received_input_;
    }

private:
    struct inflate_step_result {
        std::size_t consumed_{0};
        std::size_t produced_{0};
        int status_{Z_OK};
    };
    struct active final {};
    struct gzip_member_boundary final {};
    struct completed final {};
    using state = std::variant<active, gzip_member_boundary, completed>;
    [[nodiscard]] inflate_step_result inflate_step(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result need_input(std::size_t consumed) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result output(std::size_t consumed, std::string_view bytes) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result complete(std::size_t consumed) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result fail(std::size_t consumed, http_transfer_coding_decode_error error) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result fail_decoder(std::size_t consumed) noexcept;
    zlib_inflate_stream stream_;
    state state_{active{}};
    protocol_byte_limit body_limit_;
    std::size_t decoded_bytes_{0};
    // An empty coded body carries no content and completes at framing EOF.
    bool received_input_{false};
    http_transfer_coding coding_;
};

namespace {

[[nodiscard]] int transfer_coding_window_bits(http_transfer_coding coding) {
    switch (coding) {
        case http_transfer_coding::gzip:
            return 15 + 16;
        case http_transfer_coding::deflate:
            return 15;
        default:
            throw std::invalid_argument("unsupported HTTP transfer coding");
    }
}

}  // namespace

transfer_coding_decoder::transfer_coding_decoder(
    http_transfer_coding coding, protocol_byte_limit decoded_limit)
    : stream_(transfer_coding_window_bits(coding)),
      body_limit_(decoded_limit),
      coding_(coding) {
    if (!stream_.initialized()) {
        throw std::runtime_error("failed to initialize transfer-coding decoder");
    }
}

http_transfer_coding_decode_result transfer_coding_decoder::decode(
    std::string_view input, std::span<char> output_buffer) noexcept {
    if (std::holds_alternative<completed>(state_)) {
        return input.empty() ? complete(0) : fail(0, http_transfer_coding_decode_error::invalid_content);
    }

    std::size_t consumed = 0;
    std::size_t produced = 0;
    for (;;) {
        if (std::holds_alternative<gzip_member_boundary>(state_)) {
            if (consumed == input.size()) {
                return produced != 0
                           ? output(consumed, std::string_view(output_buffer.data(), produced))
                           : need_input(consumed);
            }
            if (!stream_.reset()) {
                return fail_decoder(consumed);
            }
            state_.emplace<active>();
        }

        const auto window_bytes =
            std::min<std::size_t>(input.size() - consumed, (std::numeric_limits<uInt>::max)());
        const auto step = inflate_step(input.substr(consumed), output_buffer.subspan(produced));
        consumed += step.consumed_;
        received_input_ = received_input_ || step.consumed_ != 0;
        if (body_limit_.addition_exceeds(decoded_bytes_, step.produced_)) {
            return fail(consumed, http_transfer_coding_decode_error::decoded_size_exceeded);
        }
        decoded_bytes_ += step.produced_;
        produced += step.produced_;

        if (step.status_ == Z_STREAM_END) {
            if (coding_ != http_transfer_coding::gzip) {
                if (consumed != input.size()) {
                    return fail(consumed, http_transfer_coding_decode_error::invalid_content);
                }
                state_.emplace<completed>();
                return produced != 0
                           ? output(consumed, std::string_view(output_buffer.data(), produced))
                           : complete(consumed);
            }

            // RFC 1952 gzip data is a series of members. A member boundary is
            // not the end of the transfer coding: another member can arrive in
            // the same HTTP chunk or in a later one. Only framing EOF, reported
            // through finish_input(), commits this boundary as complete.
            state_.emplace<gzip_member_boundary>();
            if (produced == output_buffer.size()) {
                return output(consumed, std::string_view(output_buffer.data(), produced));
            }
            continue;
        }

        if (step.status_ != Z_OK && step.status_ != Z_BUF_ERROR) {
            if (step.status_ == Z_DATA_ERROR || step.status_ == Z_NEED_DICT) {
                return fail(consumed, http_transfer_coding_decode_error::invalid_content);
            }
            return fail_decoder(consumed);
        }

        if (produced != 0) {
            return output(consumed, std::string_view(output_buffer.data(), produced));
        }
        if (consumed != input.size()) {
            // zlib sees at most one uInt window per step; a window that was
            // consumed without output (RFC 1952 FNAME/FCOMMENT bytes) is not
            // corrupt, the remaining input needs another step.
            if (step.consumed_ == window_bytes) {
                continue;
            }
            return fail(consumed, http_transfer_coding_decode_error::invalid_content);
        }
        return need_input(consumed);
    }
}

http_transfer_coding_decode_result transfer_coding_decoder::finish_input() noexcept {
    if (std::holds_alternative<completed>(state_) ||
        std::holds_alternative<gzip_member_boundary>(state_) ||
        (std::holds_alternative<active>(state_) && !received_input_)) {
        state_.emplace<completed>();
        return complete(0);
    }
    return fail(0, http_transfer_coding_decode_error::invalid_content);
}

transfer_coding_decoder::inflate_step_result transfer_coding_decoder::inflate_step(
    std::string_view input, std::span<char> output) noexcept {
    const auto input_bytes = std::min<std::size_t>(input.size(), (std::numeric_limits<uInt>::max)());
    auto& stream = stream_.native();
    stream.next_in =
        input_bytes == 0 ? Z_NULL : reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input_bytes);
    const auto output_bytes =
        std::min<std::size_t>(output.size(), (std::numeric_limits<uInt>::max)());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output_bytes);

    const auto status = inflate(&stream, Z_NO_FLUSH);
    const auto consumed = input_bytes - stream.avail_in;
    const auto produced = output_bytes - stream.avail_out;
    stream.next_in = Z_NULL;
    stream.avail_in = 0;
    stream.next_out = Z_NULL;
    stream.avail_out = 0;
    return inflate_step_result{consumed, produced, status};
}

http_transfer_coding_decode_result transfer_coding_decoder::need_input(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(consumed));
}

http_transfer_coding_decode_result transfer_coding_decoder::output(
    std::size_t consumed, std::string_view bytes_value) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_output_view(consumed, bytes_value));
}

http_transfer_coding_decode_result transfer_coding_decoder::complete(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(consumed));
}

http_transfer_coding_decode_result transfer_coding_decoder::fail(
    std::size_t consumed, http_transfer_coding_decode_error error) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_failure(consumed, error));
}

http_transfer_coding_decode_result transfer_coding_decoder::fail_decoder(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(consumed));
}

}  // namespace detail

namespace {

constexpr std::size_t transfer_coding_scratch_bytes = 4096;

struct transfer_coding_scratch_buffer final {
    // Stage output bounds valid bytes to [begin_, end_), so default construction
    // leaves the 4 KiB storage uninitialized instead of zero-filling it.
    std::array<char, transfer_coding_scratch_bytes> bytes_;
};

struct transfer_coding_stage final {
    transfer_coding_stage(http_transfer_coding coding, protocol_byte_limit decoded_limit)
        : decoder_(coding, decoded_limit) {}

    detail::transfer_coding_decoder decoder_;
    std::size_t begin_{0};
    std::size_t end_{0};
    bool complete_{false};
};

}  // namespace

struct http_transfer_coding_stack_decoder::impl_type final {
    impl_type(std::span<const http_transfer_coding> codings, protocol_byte_limit decoded_limit)
        : stage_count_(codings.size()) {
        std::size_t index = 0;
        for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
            stages_[index++].emplace(*coding, decoded_limit);
        }
    }
    std::array<std::optional<transfer_coding_stage>, max_transfer_codings> stages_{};
    std::size_t stage_count_{0};
    // Only inter-stage edges own scratch. The final stage writes directly to
    // the caller's buffer, so a one-coding sequence uses no internal scratch.
    std::array<transfer_coding_scratch_buffer, max_transfer_codings - 1> buffers_;
    std::optional<http_transfer_coding_decode_error> failure_;
    bool decoder_failed_{false};
};

http_transfer_coding_stack_decoder::http_transfer_coding_stack_decoder(
    std::span<const http_transfer_coding> codings, protocol_byte_limit decoded_limit) {
    if (codings.empty() || codings.size() > max_transfer_codings) {
        throw std::invalid_argument("invalid transfer-coding stack length");
    }
    impl_ = new impl_type(codings, decoded_limit);
}

http_transfer_coding_stack_decoder::~http_transfer_coding_stack_decoder() {
    delete impl_;
}

http_transfer_coding_decode_result http_transfer_coding_stack_decoder::decode(
    std::string_view input, std::span<char> output_buffer) noexcept {
    const auto failed = [this](std::size_t consumed) {
        if (impl_->decoder_failed_) {
            return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(consumed));
        }
        return http_transfer_coding_decode_result(
            http_transfer_coding_decode_failure(consumed, *impl_->failure_));
    };
    if (impl_->decoder_failed_ || impl_->failure_.has_value()) {
        return failed(0);
    }
    if (output_buffer.empty()) {
        impl_->decoder_failed_ = true;
        return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
    }

    std::size_t wire_consumed = 0;
    for (;;) {
        bool progressed = false;
        for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
            auto& stage = *impl_->stages_[index];
            if (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_) {
                continue;
            }
            std::string_view layer_input;
            if (index == 0) {
                layer_input = input.substr(wire_consumed);
            } else {
                auto& previous = *impl_->stages_[index - 1];
                layer_input = std::string_view(
                    impl_->buffers_[index - 1].bytes_.data() + previous.begin_,
                    previous.end_ - previous.begin_);
            }
            if (stage.complete_) {
                if (!layer_input.empty()) {
                    impl_->failure_ = http_transfer_coding_decode_error::invalid_content;
                    return failed(wire_consumed);
                }
                continue;
            }
            const auto output = index + 1 == impl_->stage_count_
                                    ? output_buffer
                                    : std::span<char>(impl_->buffers_[index].bytes_);
            const auto decoded = stage.decoder_.decode(layer_input, output);
            const auto consumed = decoded.consumed_bytes();
            if (index == 0) {
                wire_consumed += consumed;
            } else {
                auto& previous = *impl_->stages_[index - 1];
                previous.begin_ += consumed;
                if (previous.begin_ == previous.end_) {
                    previous.begin_ = 0;
                    previous.end_ = 0;
                }
            }
            progressed = progressed || consumed != 0;
            if (const auto* bytes = decoded.output()) {
                progressed = true;
                if (index + 1 == impl_->stage_count_) {
                    return http_transfer_coding_decode_result(
                        http_transfer_coding_decode_output_view(wire_consumed, bytes->bytes()));
                }
                stage.begin_ = 0;
                stage.end_ = bytes->bytes().size();
                continue;
            }
            if (const auto* protocol_failure = decoded.failure()) {
                impl_->failure_ = protocol_failure->error();
                return failed(wire_consumed);
            }
            if (decoded.decoder_failure() != nullptr) {
                impl_->decoder_failed_ = true;
                return failed(wire_consumed);
            }
            if (decoded.complete() != nullptr) {
                stage.complete_ = true;
                progressed = true;
            }
        }

        bool all_complete = true;
        bool pending = false;
        for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
            const auto& stage = *impl_->stages_[index];
            all_complete = all_complete && stage.complete_;
            pending = pending || (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_);
        }
        if (all_complete && !pending) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(wire_consumed));
        }
        if (!progressed) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(wire_consumed));
        }
    }
}

http_transfer_coding_decode_result http_transfer_coding_stack_decoder::finish_input() noexcept {
    if (impl_->decoder_failed_) {
        return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
    }
    if (impl_->failure_.has_value()) {
        return http_transfer_coding_decode_result(
            http_transfer_coding_decode_failure(0, *impl_->failure_));
    }
    const bool wire_received_input = impl_->stages_[0]->decoder_.received_input();
    for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
        auto& stage = *impl_->stages_[index];
        if (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(0));
        }
        if (stage.complete_) {
            continue;
        }
        if (index > 0 && wire_received_input && !stage.decoder_.received_input()) {
            impl_->failure_ = http_transfer_coding_decode_error::invalid_content;
            return http_transfer_coding_decode_result(
                http_transfer_coding_decode_failure(0, *impl_->failure_));
        }
        const auto finished = stage.decoder_.finish_input();
        if (const auto* protocol_failure = finished.failure()) {
            impl_->failure_ = protocol_failure->error();
            return http_transfer_coding_decode_result(
                http_transfer_coding_decode_failure(0, *impl_->failure_));
        }
        if (finished.decoder_failure() != nullptr) {
            impl_->decoder_failed_ = true;
            return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
        }
        stage.complete_ = true;
    }
    return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(0));
}

}  // namespace ruvia

namespace ruvia {

std::string_view http1_client_response_body_error_message(http1_client_response_body_error error) noexcept {
    switch (error) {
        case http1_client_response_body_error::invalid_framing:
            return "invalid HTTP/1 response body framing";
        case http1_client_response_body_error::invalid_transfer_coding:
            return "invalid HTTP/1 response transfer coding";
        case http1_client_response_body_error::incomplete_body:
            return "incomplete HTTP/1 response body";
    }
    return "invalid HTTP/1 response body";
}

http1_client_response_body_decoder::http1_client_response_body_decoder(
    http1_client_response_plan plan) {
    if (plan.informational() || plan.connect_tunnel() || plan.protocol_upgrade()) {
        throw std::invalid_argument("HTTP/1 response plan has no ordinary message body");
    }
    if (const auto* without = plan.without_content()) {
        framing_ = framing_type::no_body;
        persistence_ = without->persistence();
        return;
    }
    // A discarding (205) plan carries the same framings as an ordinary one,
    // but its content is never emitted, so it is never transfer-decoded.
    discard_content_ = plan.discards_content();
    const auto configure_coding = [this](const http_transfer_codings& codings) {
        if (!codings.empty() && !discard_content_) {
            transfer_.emplace(std::span<const http_transfer_coding>(
                                  codings.data(), codings.size()),
                protocol_byte_limit::unlimited());
        }
    };
    if (const auto* known = plan.known_length()) {
        framing_ = framing_type::fixed;
        remaining_ = known->content_length();
        persistence_ = known->persistence();
    } else if (const auto* chunked = plan.chunked()) {
        framing_ = framing_type::chunked;
        persistence_ = chunked->persistence();
        configure_coding(chunked->transfer_codings());
        chunked_.emplace(http1_chunked_body_decoder_config{.obs_fold_ = http1_trailer_obs_fold::accept,
            .trailer_message_ = http1_trailer_message::response});
    } else if (const auto* close = plan.close_delimited()) {
        framing_ = framing_type::close_delimited;
        configure_coding(close->transfer_codings());
    } else {
        throw std::invalid_argument("unsupported HTTP/1 response plan");
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::decode(
    std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replay_terminal();
    }
    return step(available, scratch, eof_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::finish_input(
    std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replay_terminal();
    }
    eof_ = true;
    return step(available, scratch, true);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step(
    std::string_view available, std::span<char> scratch, bool eof) {
    if (!discard_content_) {
        return step_content(available, scratch, eof);
    }
    // RFC 9110 section 15.3.6 forbids only generating 205 content: consume
    // what the framing delimits without emitting it, then report the next
    // non-output result over every byte consumed by this call.
    std::size_t discarded = 0;
    for (;;) {
        const auto result_value = step_content(available.substr(discarded), scratch, eof);
        if (result_value.output() == nullptr) {
            return with_consumed_prefix(result_value, discarded);
        }
        discarded += result_value.consumed_bytes();
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_content(
    std::string_view available, std::span<char> scratch, bool eof) {
    if (trailers_reported_) {
        return finish_transfer(0, scratch);
    }
    // A full output step can leave inflate output buffered after consuming
    // all wire input. Drain it before asking framing or transport for more.
    if (transfer_ && transfer_phase_ == transfer_phase_type::drain_output) {
        const auto pending = drive_transfer({}, 0, scratch);
        if (pending.need_input() == nullptr) {
            return pending;
        }
    }
    switch (framing_) {
        case framing_type::fixed:
            return step_fixed(available, scratch, eof);
        case framing_type::close_delimited:
            return step_close_delimited(available, scratch, eof);
        case framing_type::chunked:
            return step_chunked(available, scratch, eof);
        case framing_type::no_body:
            break;
    }
    return complete(0);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_fixed(
    std::string_view available, std::span<char> scratch, bool eof) {
    if (remaining_ == 0) {
        return complete(0);
    }
    if (available.empty()) {
        return need_input_or_incomplete(0, eof);
    }
    const auto output_length = std::min({remaining_, available.size(), scratch.size()});
    remaining_ -= output_length;
    return emit(output_length, available.substr(0, output_length));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_close_delimited(
    std::string_view available, std::span<char> scratch, bool eof) {
    if (!transfer_) {
        if (available.empty()) {
            return eof ? complete(0) : need_input(0);
        }
        const auto output_length = std::min(available.size(), scratch.size());
        return emit(output_length, available.substr(0, output_length));
    }
    std::size_t consumed = 0;
    for (;;) {
        const auto rest = available.substr(consumed);
        if (transfer_phase_ == transfer_phase_type::ended) {
            // The connection close is the only valid framing end after the
            // transfer coding completed.
            if (!rest.empty()) {
                return fail(http1_client_response_body_error::invalid_transfer_coding, consumed);
            }
            return eof ? complete(consumed) : need_input(consumed);
        }
        if (rest.empty()) {
            return eof ? finish_transfer(consumed, scratch) : need_input(consumed);
        }
        const auto driven = drive_transfer(rest, consumed, scratch);
        const auto input_consumed = driven.consumed_bytes() - consumed;
        consumed += input_consumed;
        if (driven.need_input() == nullptr) {
            return driven;
        }
        if (input_consumed == 0 && transfer_phase_ != transfer_phase_type::ended) {
            return need_input_or_incomplete(consumed, eof);
        }
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_chunked(
    std::string_view available, std::span<char> scratch, bool eof) {
    std::size_t consumed = 0;
    for (;;) {
        if (auto pending = drain_pending_chunk(available, consumed, scratch, eof)) {
            return *pending;
        }
        const auto rest = available.substr(consumed);
        const auto decoded = chunked_->decode(rest,
            transfer_ ? (std::numeric_limits<std::size_t>::max)() : scratch.size());
        const auto chunk_consumed = consumed + decoded.consumed_bytes();
        if (decoded.failure() != nullptr) {
            return fail(http1_client_response_body_error::invalid_framing, chunk_consumed);
        }
        if (decoded.need_more() != nullptr) {
            return need_input_or_incomplete(chunk_consumed, eof);
        }
        if (const auto* complete = decoded.complete()) {
            trailers_reported_ = true;
            return validated_trailers(chunk_consumed, complete->trailers());
        }
        const auto body = decoded.body_chunk()->bytes();
        if (!transfer_) {
            return emit(chunk_consumed, body);
        }
        // The chunk decoder already consumed the whole chunk, but the transfer
        // decoder may take only part of its data. Track the rest as wire bytes
        // so the caller keeps the unconsumed suffix without a borrowed view.
        consumed += static_cast<std::size_t>(body.data() - rest.data());
        pending_body_bytes_ = body.size();
        pending_delimiter_bytes_ = chunk_consumed - consumed - body.size();
    }
}

// Feeds pending chunk data to the transfer decoder and then skips the chunk
// delimiter. Returns nothing once both are consumed and framing can continue.
std::optional<http1_client_response_body_decoder::result_type>
http1_client_response_body_decoder::drain_pending_chunk(
    std::string_view available, std::size_t& consumed, std::span<char> scratch, bool eof) {
    if (pending_body_bytes_ != 0) {
        const auto input = available.substr(consumed, std::min(pending_body_bytes_, available.size() - consumed));
        const auto driven = drive_transfer(input, consumed, scratch);
        const auto input_consumed = driven.consumed_bytes() - consumed;
        pending_body_bytes_ -= input_consumed;
        consumed += input_consumed;
        if (pending_body_bytes_ == 0) {
            consume_pending_delimiter(available, consumed);
        }
        if (driven.need_input() == nullptr) {
            return with_consumed(driven, consumed);
        }
        if (pending_body_bytes_ != 0 || consumed == available.size()) {
            return need_input_or_incomplete(consumed, eof);
        }
    } else {
        consume_pending_delimiter(available, consumed);
    }
    if (pending_delimiter_bytes_ != 0) {
        return need_input_or_incomplete(consumed, eof);
    }
    return std::nullopt;
}

// The chunk decoder already validated these delimiter bytes.
void http1_client_response_body_decoder::consume_pending_delimiter(
    std::string_view available, std::size_t& consumed) noexcept {
    const auto bytes = std::min(pending_delimiter_bytes_, available.size() - consumed);
    consumed += bytes;
    pending_delimiter_bytes_ -= bytes;
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::drive_transfer(
    std::string_view input, std::size_t wire_prefix, std::span<char> scratch) {
    const auto decoded = transfer_->decode(input, scratch);
    const auto consumed = wire_prefix + decoded.consumed_bytes();
    if (const auto* output = decoded.output()) {
        return emit_transfer_output(consumed, output->bytes());
    }
    if (decoded.need_input() != nullptr) {
        transfer_phase_ = transfer_phase_type::needs_framed_input;
        return need_input(consumed);
    }
    if (decoded.complete() != nullptr) {
        transfer_phase_ = transfer_phase_type::ended;
        return need_input(consumed);
    }
    if (decoded.failure() != nullptr) {
        return fail(http1_client_response_body_error::invalid_transfer_coding, consumed);
    }
    return decoder_failure(consumed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::finish_transfer(
    std::size_t consumed, std::span<char> scratch) {
    if (!transfer_) {
        return complete(consumed);
    }
    // Buffered output drains before framing EOF is committed to the decoder.
    auto decoded = transfer_->decode({}, scratch);
    if (decoded.need_input() != nullptr) {
        decoded = transfer_->finish_input();
    }
    if (const auto* output = decoded.output()) {
        return emit_transfer_output(consumed, output->bytes());
    }
    if (decoded.complete() != nullptr) {
        transfer_phase_ = transfer_phase_type::ended;
        return complete(consumed);
    }
    if (decoded.decoder_failure() != nullptr) {
        return decoder_failure(consumed);
    }
    return fail(http1_client_response_body_error::incomplete_body, consumed);
}

http1_client_response_body_decoder::result_type
http1_client_response_body_decoder::replay_terminal() const noexcept {
    return std::visit([](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        if constexpr (std::is_same_v<t_type, complete_type>) {
            return result_type(complete_type(0, payload_value.persistence()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(0, payload_value.error()));
        } else {
            static_assert(std::is_same_v<t_type, decoder_failure_type>);
            return result_type(decoder_failure_type(0));
        }
    },
        *terminal_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::need_input(
    std::size_t consumed) const noexcept {
    return result_type(need_input_type(consumed));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::need_input_or_incomplete(
    std::size_t consumed, bool eof) {
    if (eof) {
        return fail(http1_client_response_body_error::incomplete_body, consumed);
    }
    return need_input(consumed);
}

// The single emission point.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::emit(
    std::size_t consumed, std::string_view bytes_value) {
    return result_type(output_view_type(consumed, bytes_value));
}

// Transfer output borrows scratch and must be drained before more framed input.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::emit_transfer_output(
    std::size_t consumed, std::string_view bytes_value) {
    transfer_phase_ = transfer_phase_type::drain_output;
    return emit(consumed, bytes_value);
}

// Re-reports a transfer result with the wire bytes consumed after its input,
// such as the chunk delimiter that follows the final chunk data.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::with_consumed(
    const result_type& result, std::size_t consumed) noexcept {
    return std::visit([consumed](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        if constexpr (std::is_same_v<t_type, output_view_type>) {
            return result_type(output_view_type(consumed, payload_value.bytes()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(consumed, payload_value.error()));
        } else if constexpr (std::is_same_v<t_type, decoder_failure_type>) {
            return result_type(decoder_failure_type(consumed));
        } else {
            return result_type(payload_value);
        }
    },
        result.value_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::with_consumed_prefix(
    const result_type& result, std::size_t prefix) noexcept {
    return std::visit([prefix](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        const auto consumed = prefix + payload_value.consumed_bytes();
        if constexpr (std::is_same_v<t_type, need_input_type>) {
            return result_type(need_input_type(consumed));
        } else if constexpr (std::is_same_v<t_type, output_view_type>) {
            return result_type(output_view_type(consumed, payload_value.bytes()));
        } else if constexpr (std::is_same_v<t_type, validated_trailers_view_type>) {
            return result_type(validated_trailers_view_type(consumed, payload_value.fields()));
        } else if constexpr (std::is_same_v<t_type, complete_type>) {
            return result_type(complete_type(consumed, payload_value.persistence()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(consumed, payload_value.error()));
        } else {
            static_assert(std::is_same_v<t_type, decoder_failure_type>);
            return result_type(decoder_failure_type(consumed));
        }
    },
        result.value_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::validated_trailers(
    std::size_t consumed, std::span<const http_header> fields) {
    return result_type(validated_trailers_view_type(consumed, fields));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::complete(
    std::size_t consumed) {
    const auto completed = complete_type(consumed, persistence_);
    terminal_ = completed;
    return result_type(completed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::fail(
    http1_client_response_body_error error, std::size_t consumed) {
    const auto failure = protocol_failure_type(consumed, error);
    terminal_ = failure;
    return result_type(failure);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::decoder_failure(
    std::size_t consumed) {
    const auto failure = decoder_failure_type(consumed);
    terminal_ = failure;
    return result_type(failure);
}

std::size_t http1_client_response_body_decoder::result_type::consumed_bytes() const noexcept {
    return std::visit([](const auto& payload_value) { return payload_value.consumed_bytes(); }, value_);
}

const http1_client_response_body_decoder::need_input_type*
http1_client_response_body_decoder::result_type::need_input() const& noexcept {
    return std::get_if<need_input_type>(&value_);
}

const http1_client_response_body_decoder::output_view_type*
http1_client_response_body_decoder::result_type::output() const& noexcept {
    return std::get_if<output_view_type>(&value_);
}

const http1_client_response_body_decoder::validated_trailers_view_type*
http1_client_response_body_decoder::result_type::trailers() const& noexcept {
    return std::get_if<validated_trailers_view_type>(&value_);
}

const http1_client_response_body_decoder::complete_type*
http1_client_response_body_decoder::result_type::complete() const& noexcept {
    return std::get_if<complete_type>(&value_);
}

const http1_client_response_body_decoder::protocol_failure_type*
http1_client_response_body_decoder::result_type::protocol_failure() const& noexcept {
    return std::get_if<protocol_failure_type>(&value_);
}

const http1_client_response_body_decoder::decoder_failure_type*
http1_client_response_body_decoder::result_type::decoder_failure() const& noexcept {
    return std::get_if<decoder_failure_type>(&value_);
}

}  // namespace ruvia

// The Close frame payload (RFC 6455 section 5.5.1): encoding the one an
// endpoint sends (a validated status code followed by an optional UTF-8
// reason, capped at the 125-byte control-frame limit) and checking the one a
// peer sent.

namespace ruvia::detail {

enum class websocket_close_payload_encode_error : std::uint8_t {
    invalid_reason,
    reason_too_large,
};

class websocket_close_payload_encode_result;

class websocket_encoded_close_payload final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
        return std::string_view(bytes_.data(), size_);
    }
    [[nodiscard]] constexpr std::string_view bytes() const&& = delete;

private:
    friend class websocket_close_payload_encode_result;
    friend websocket_close_payload_encode_result encode_websocket_close_payload(
        websocket_close_code, std::string_view) noexcept;

    websocket_encoded_close_payload(websocket_close_code code, std::string_view reason) noexcept;

    std::array<char, 125> bytes_;
    std::uint8_t size_{0};
};

class websocket_close_payload_encode_failure final {
public:
    [[nodiscard]] constexpr websocket_close_payload_encode_error error() const noexcept {
        return error_;
    }

private:
    friend class websocket_close_payload_encode_result;
    friend websocket_close_payload_encode_result encode_websocket_close_payload(
        websocket_close_code, std::string_view) noexcept;

    explicit constexpr websocket_close_payload_encode_failure(
        websocket_close_payload_encode_error error) noexcept
        : error_(error) {}

    websocket_close_payload_encode_error error_;
};

class websocket_close_payload_encode_result final {
public:
    [[nodiscard]] constexpr const websocket_encoded_close_payload* encoded() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] constexpr const websocket_encoded_close_payload* encoded() const&& = delete;

    [[nodiscard]] constexpr const websocket_close_payload_encode_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] constexpr const websocket_close_payload_encode_failure* failure() const&& = delete;

private:
    friend websocket_close_payload_encode_result encode_websocket_close_payload(
        websocket_close_code, std::string_view) noexcept;

    explicit websocket_close_payload_encode_result(websocket_encoded_close_payload encoded) noexcept
        : value_(encoded) {}

    explicit constexpr websocket_close_payload_encode_result(
        websocket_close_payload_encode_failure failure) noexcept
        : value_(failure) {}

    using value_type = std::variant<websocket_encoded_close_payload, websocket_close_payload_encode_failure>;
    value_type value_;
};

[[nodiscard]] websocket_close_payload_encode_result encode_websocket_close_payload(
    websocket_close_code code, std::string_view reason) noexcept;

// The protocol failure a received Close payload commits, or nullopt when it is legal.
[[nodiscard]] std::optional<websocket_protocol_failure> websocket_close_payload_failure(
    std::string_view payload_value) noexcept;
}  // namespace ruvia::detail

namespace ruvia::detail {

websocket_encoded_close_payload::websocket_encoded_close_payload(
    websocket_close_code code, std::string_view reason) noexcept
    : size_(static_cast<std::uint8_t>(reason.size() + 2)) {
    const auto code_value = code.value();
    bytes_[0] = static_cast<char>((code_value >> 8) & 0xFF);
    bytes_[1] = static_cast<char>(code_value & 0xFF);
    if (!reason.empty()) {
        std::memcpy(bytes_.data() + 2, reason.data(), reason.size());
    }
}

websocket_close_payload_encode_result encode_websocket_close_payload(
    websocket_close_code code, std::string_view reason) noexcept {
    if (reason.size() > 123) {
        return websocket_close_payload_encode_result(
            websocket_close_payload_encode_failure(websocket_close_payload_encode_error::reason_too_large));
    }
    if (!::ruvia::is_valid_utf8(reason)) {
        return websocket_close_payload_encode_result(
            websocket_close_payload_encode_failure(websocket_close_payload_encode_error::invalid_reason));
    }
    return websocket_close_payload_encode_result(websocket_encoded_close_payload(code, reason));
}

std::optional<websocket_protocol_failure> websocket_close_payload_failure(
    std::string_view payload_value) noexcept {
    // A malformed frame is a protocol error (close 1002); an otherwise-valid
    // frame whose reason is not valid UTF-8 is invalid payload data (close
    // 1007, RFC 6455 section 8.1).
    if (payload_value.size() == 1) {
        return websocket_protocol_failure::protocol_error;
    }
    if (payload_value.size() < 2) {
        return std::nullopt;
    }
    if (!websocket_close_code::is_valid_value(read_websocket_uint16(payload_value.data()))) {
        return websocket_protocol_failure::protocol_error;
    }
    if (!::ruvia::is_valid_utf8(payload_value.substr(2))) {
        return websocket_protocol_failure::invalid_payload_data;
    }
    return std::nullopt;
}

class websocket_frame_read_result;

// One borrowed frame published by the wire reader, which owns masking, length,
// opcode/compression combinations, and Close payload validation. Payload
// storage must outlive the view.
class websocket_frame_view final {
public:
    [[nodiscard]] constexpr websocket_frame_kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] constexpr bool final() const noexcept {
        return final_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return compressed_;
    }

private:
    friend class websocket_frame_read_result;

    constexpr websocket_frame_view(
        const websocket_frame_start& start, std::string_view payload_value) noexcept
        : kind_(start.kind()),
          payload_(payload_value),
          final_(start.final()),
          compressed_(start.compressed()) {}

    websocket_frame_kind kind_;
    std::string_view payload_;
    bool final_;
    bool compressed_;
};

// Drop the bytes the previously returned frame occupied. Consuming never moves
// bytes; the buffer moves the unread tail only when a later read needs room.
// A drained buffer keeps at most websocket_retained_buffer_bytes, so a burst
// of large messages does not stay pinned to the connection.
inline void consume_websocket_read_frame(http_io_buffer& buffer, std::size_t& pending_consume) noexcept {
    if (pending_consume == 0) {
        return;
    }
    buffer.consume(std::exchange(pending_consume, 0));
    if (buffer.empty()) {
        buffer.clear_retaining_small(websocket_retained_buffer_bytes);
    }
}

class websocket_frame_need_input final {
public:
    // Bytes the frame still lacks once its header is complete; 0 while the
    // header itself is incomplete. The header length is already within the
    // message limit.
    [[nodiscard]] constexpr std::size_t missing_bytes() const noexcept {
        return missing_bytes_;
    }

private:
    friend class websocket_frame_read_result;
    explicit constexpr websocket_frame_need_input(std::size_t missing_bytes) noexcept
        : missing_bytes_(missing_bytes) {}

    std::size_t missing_bytes_;
};

class websocket_frame_read_failure final {
public:
    [[nodiscard]] constexpr websocket_protocol_failure error() const noexcept {
        return error_;
    }

private:
    friend class websocket_frame_read_result;

    explicit constexpr websocket_frame_read_failure(websocket_protocol_failure error) noexcept
        : error_(error) {}

    websocket_protocol_failure error_;
};

// Parsing either needs more input, exposes one complete borrowed frame, or reports
// one typed protocol failure. No default frame, EOF boolean, or wire-format
// exception can coexist with another outcome; only need_input() carries a
// byte count.
class websocket_frame_read_result final {
public:
    [[nodiscard]] constexpr const websocket_frame_need_input* need_input() const& noexcept {
        return std::get_if<websocket_frame_need_input>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_need_input* need_input() const&& = delete;

    [[nodiscard]] constexpr const websocket_frame_view* frame() const& noexcept {
        return std::get_if<websocket_frame_view>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_view* frame() const&& = delete;

    [[nodiscard]] constexpr const websocket_frame_read_failure* failure() const& noexcept {
        return std::get_if<websocket_frame_read_failure>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_read_failure* failure() const&& = delete;

private:
    friend websocket_frame_read_result websocket_try_read_frame(
        http_io_buffer&, std::size_t&, protocol_byte_limit, bool, bool);

    using value_type =
        std::variant<websocket_frame_need_input, websocket_frame_view, websocket_frame_read_failure>;

    template <typename alternative_type>
    explicit constexpr websocket_frame_read_result(alternative_type alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr websocket_frame_read_result make_need_input(
        std::size_t missing_bytes = 0) noexcept {
        return websocket_frame_read_result(websocket_frame_need_input(missing_bytes));
    }

    [[nodiscard]] static constexpr websocket_frame_read_result make_frame(
        const websocket_frame_start& start, std::string_view payload_value) noexcept {
        return websocket_frame_read_result(websocket_frame_view(start, payload_value));
    }

    [[nodiscard]] static constexpr websocket_frame_read_result make_failure(
        websocket_protocol_failure error) noexcept {
        return websocket_frame_read_result(websocket_frame_read_failure(error));
    }

    value_type value_;
};

// Single owner of websocket frame decode (RFC 6455 §5.2): FIN/opcode/length
// parsing, role-correct masking validation, control-frame and length-limit
// validation, and in-place unmasking when the peer is a client.
// It never performs I/O and never throws for peer bytes; callers append transport
// input after need_input(), while failure() carries the Close reason. A returned
// frame stays in the buffer, counted by `pending_consume`, until the next call.
[[nodiscard]] inline websocket_frame_read_result websocket_try_read_frame(http_io_buffer& buffer,
    std::size_t& pending_consume, protocol_byte_limit message_limit, bool permessage_deflate, bool expect_masked) {
    consume_websocket_read_frame(buffer, pending_consume);
    const auto available = buffer.size();
    if (available < 2) {
        return websocket_frame_read_result::make_need_input();
    }
    const auto first = static_cast<unsigned char>(buffer.data()[0]);
    const auto second = static_cast<unsigned char>(buffer.data()[1]);
    std::uint64_t length = second & 0x7FU;
    std::size_t header_size = 2;

    const auto frame_start =
        decode_websocket_frame_start(first, second, permessage_deflate, expect_masked);
    if (!frame_start.has_value()) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    // RFC 6455 section 5.2 obliges the sender to use the minimal length form;
    // the frame stays unambiguous either way, so a receiver does not reject a
    // longer form (sender MUST, not recipient rule).
    if (length == 126) {
        if (available < header_size + 2) {
            return websocket_frame_read_result::make_need_input();
        }
        length = read_websocket_uint16(buffer.data() + header_size);
        header_size += 2;
    } else if (length == 127) {
        if (available < header_size + 8) {
            return websocket_frame_read_result::make_need_input();
        }
        const auto decoded_length = read_websocket_uint64(
            std::span<const char, 8>(buffer.data() + header_size, 8));
        if ((decoded_length.index() != 0)) {
            return websocket_frame_read_result::make_failure(std::get<1>(decoded_length));
        }
        length = std::get<0>(decoded_length);
        header_size += 8;
    }

    if (is_invalid_websocket_control_frame(*frame_start, length)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    if (websocket_frame_exceeds_message_limit(frame_start->kind(), length, message_limit)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::message_too_large);
    }
    const auto mask_bytes = expect_masked ? std::size_t{4} : std::size_t{0};
    // header_size is at most 10, so only the payload length can overflow.
    if (length > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)() - header_size - mask_bytes)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    const auto total_frame_bytes = header_size + mask_bytes + static_cast<std::size_t>(length);
    if (available < total_frame_bytes) {
        return websocket_frame_read_result::make_need_input(total_frame_bytes - available);
    }

    const auto mask_offset = header_size;
    const auto payload_offset = mask_offset + mask_bytes;
    const auto payload_size = static_cast<std::size_t>(length);
    auto* payload_value = buffer.data() + payload_offset;
    if (expect_masked) {
        decode_masked_websocket_payload(payload_value, payload_size, buffer.data() + mask_offset);
    }
    const auto payload_view = std::string_view(payload_value, payload_size);
    if (frame_start->kind() == websocket_frame_kind::close) {
        if (const auto failure = websocket_close_payload_failure(payload_view)) {
            return websocket_frame_read_result::make_failure(*failure);
        }
    }
    pending_consume = total_frame_bytes;
    return websocket_frame_read_result::make_frame(*frame_start, payload_view);
}

class websocket_inbound_result;

class websocket_inbound_continue final {
private:
    friend class websocket_inbound_result;
    constexpr websocket_inbound_continue() noexcept = default;
};

class websocket_inbound_control_frame final {
public:
    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class websocket_inbound_result;

    constexpr websocket_inbound_control_frame(
        websocket_opcode opcode, std::string_view payload_value) noexcept
        : opcode_(opcode),
          payload_(payload_value) {}

    websocket_opcode opcode_;
    std::string_view payload_;
};

enum class websocket_inbound_content_encoding : std::uint8_t {
    identity,
    per_message_deflate,
};

class websocket_inbound_message final {
public:
    [[nodiscard]] constexpr const websocket_message& message() const& noexcept {
        return message_;
    }
    [[nodiscard]] constexpr const websocket_message& message() const&& = delete;

    [[nodiscard]] constexpr websocket_inbound_content_encoding content_encoding() const noexcept {
        return content_encoding_;
    }

private:
    friend class websocket_inbound_result;

    constexpr websocket_inbound_message(
        websocket_message message, websocket_inbound_content_encoding content_encoding) noexcept
        : message_(message),
          content_encoding_(content_encoding) {}

    websocket_message message_;
    websocket_inbound_content_encoding content_encoding_;
};

class websocket_inbound_failure final {
public:
    [[nodiscard]] constexpr websocket_protocol_failure error() const noexcept {
        return error_;
    }

private:
    friend class websocket_inbound_result;

    explicit constexpr websocket_inbound_failure(websocket_protocol_failure error) noexcept
        : error_(error) {}

    websocket_protocol_failure error_;
};

// A consumed frame has exactly one semantic outcome. Control payload, application
// message, and failure code live only on their corresponding alternatives; there
// is no action enum coupled to an output parameter.
class websocket_inbound_result final {
public:
    [[nodiscard]] constexpr const websocket_inbound_continue* continue_reading() const& noexcept {
        return std::get_if<websocket_inbound_continue>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_continue* continue_reading() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_control_frame* control_frame() const& noexcept {
        return std::get_if<websocket_inbound_control_frame>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_control_frame* control_frame() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_message* message() const& noexcept {
        return std::get_if<websocket_inbound_message>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_message* message() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_failure* failure() const& noexcept {
        return std::get_if<websocket_inbound_failure>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_failure* failure() const&& = delete;

private:
    friend class websocket_inbound_assembler;

    using value_type = std::variant<websocket_inbound_continue, websocket_inbound_control_frame,
        websocket_inbound_message, websocket_inbound_failure>;

    template <typename alternative_type>
    explicit constexpr websocket_inbound_result(alternative_type alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr websocket_inbound_result make_continue() noexcept {
        return websocket_inbound_result(websocket_inbound_continue());
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_control_frame(
        websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_inbound_result(websocket_inbound_control_frame(opcode, payload_value));
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_message(
        websocket_message message, websocket_inbound_content_encoding content_encoding) noexcept {
        return websocket_inbound_result(websocket_inbound_message(message, content_encoding));
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_failure(
        websocket_protocol_failure error) noexcept {
        return websocket_inbound_result(websocket_inbound_failure(error));
    }

    value_type value_;
};

// Single owner of the websocket inbound message-reassembly state machine (RFC
// 6455 §5.4): control-frame dispatch, fragmentation across continuation frames,
// per-message size limits and UTF-8 validation. Every wire failure is returned as
// a typed alternative carrying the exact RFC Close reason; none is thrown.
struct websocket_inbound_idle final {};

class websocket_inbound_fragmented final {
public:
    constexpr websocket_inbound_fragmented(
        websocket_opcode opcode, websocket_inbound_content_encoding encoding) noexcept
        : opcode_(opcode),
          encoding_(encoding) {}

    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }
    [[nodiscard]] constexpr websocket_inbound_content_encoding encoding() const noexcept {
        return encoding_;
    }

private:
    websocket_opcode opcode_;
    websocket_inbound_content_encoding encoding_;
};

class websocket_inbound_assembler final {
public:
    websocket_inbound_assembler() = default;

    // A delivered view expires at the next poll, while a fragmented message
    // remains pinned until its final continuation or connection teardown. A
    // delivered message keeps only a bounded scratch, so streams of small
    // fragmented messages do not allocate per message and large ones are not
    // pinned to the connection.
    void release_completed() noexcept {
        if (std::holds_alternative<websocket_inbound_idle>(state_)) {
            http_clear_string_retaining_small(message_, websocket_retained_buffer_bytes);
        }
    }

    [[nodiscard]] websocket_inbound_result accept(
        const websocket_frame_view& frame, protocol_byte_limit message_limit) {
        if (is_websocket_control_frame_kind(frame.kind())) {
            const auto opcode = static_cast<websocket_opcode>(frame.kind());
            return websocket_inbound_result::make_control_frame(opcode, frame.payload());
        }
        if (frame.kind() == websocket_frame_kind::continuation) {
            const auto* fragmented = std::get_if<websocket_inbound_fragmented>(&state_);
            if (fragmented == nullptr) {
                return websocket_inbound_result::make_failure(
                    websocket_protocol_failure::protocol_error);
            }
            if (message_limit.addition_exceeds(message_.size(), frame.payload().size())) {
                return websocket_inbound_result::make_failure(
                    websocket_protocol_failure::message_too_large);
            }
            message_.append(frame.payload().data(), frame.payload().size());
            if (!frame.final()) {
                return websocket_inbound_result::make_continue();
            }
            const auto opcode = fragmented->opcode();
            const auto encoding = fragmented->encoding();
            state_.template emplace<websocket_inbound_idle>();
            const auto message = websocket_message::borrow(opcode, std::string_view(message_));
            if (encoding == websocket_inbound_content_encoding::per_message_deflate) {
                return websocket_inbound_result::make_message(
                    message, websocket_inbound_content_encoding::per_message_deflate);
            }
            if (opcode == websocket_opcode::text && !::ruvia::is_valid_utf8(message_)) {
                return websocket_inbound_result::make_failure(
                    websocket_protocol_failure::invalid_payload_data);
            }
            return websocket_inbound_result::make_message(
                message, websocket_inbound_content_encoding::identity);
        }
        if (frame.kind() == websocket_frame_kind::text ||
            frame.kind() == websocket_frame_kind::binary) {
            if (std::get_if<websocket_inbound_fragmented>(&state_) != nullptr) {
                return websocket_inbound_result::make_failure(
                    websocket_protocol_failure::protocol_error);
            }
            // websocket_try_read_frame already bounded this first frame by the message limit.
            const auto opcode = static_cast<websocket_opcode>(frame.kind());
            if (frame.final()) {
                const auto message = websocket_message::borrow(opcode, frame.payload());
                if (frame.compressed()) {
                    return websocket_inbound_result::make_message(
                        message, websocket_inbound_content_encoding::per_message_deflate);
                }
                if (opcode == websocket_opcode::text && !::ruvia::is_valid_utf8(frame.payload())) {
                    return websocket_inbound_result::make_failure(
                        websocket_protocol_failure::invalid_payload_data);
                }
                return websocket_inbound_result::make_message(
                    message, websocket_inbound_content_encoding::identity);
            }
            message_.assign(frame.payload().data(), frame.payload().size());
            state_.template emplace<websocket_inbound_fragmented>(
                opcode, frame.compressed() ? websocket_inbound_content_encoding::per_message_deflate
                                           : websocket_inbound_content_encoding::identity);
        }
        return websocket_inbound_result::make_continue();
    }

private:
    std::string message_;
    std::variant<websocket_inbound_idle, websocket_inbound_fragmented> state_;
};

}  // namespace ruvia::detail
namespace ruvia {

namespace {

enum class websocket_payload_storage : std::uint8_t {
    copy,
    borrow,
};

[[nodiscard]] constexpr websocket_termination websocket_failure_termination(
    detail::websocket_protocol_failure failure) noexcept {
    switch (failure) {
        case detail::websocket_protocol_failure::protocol_error:
            break;
        case detail::websocket_protocol_failure::invalid_payload_data:
            return websocket_termination::invalid_payload_data;
        case detail::websocket_protocol_failure::message_too_large:
            return websocket_termination::message_too_large;
    }
    return websocket_termination::protocol_error;
}

void add_saturating(std::uint64_t& total, std::size_t bytes_value) noexcept {
    const auto remaining = std::numeric_limits<std::uint64_t>::max() - total;
    total += bytes_value < remaining ? bytes_value : remaining;
}

// Where a data frame sits in its message (RFC 6455 section 5.4): only the
// first frame carries the opcode, only the last one sets FIN.
enum class websocket_fragment_position : std::uint8_t {
    whole,
    first,
    middle,
    last,
};

// UTF-8 of a text message submitted in fragments: a code point may straddle
// fragments, so the bytes of one left incomplete wait for the next fragment.
class websocket_utf8_fragments final {
public:
    [[nodiscard]] bool append(std::string_view fragment) noexcept {
        std::size_t index = 0;
        if (pending_size_ != 0) {
            const auto lead = static_cast<unsigned char>(pending_[0]);
            const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3
                                                                       : 2;
            while (pending_size_ < length && index < fragment.size()) {
                pending_[pending_size_++] = fragment[index++];
            }
            const auto sequence = scan_utf8_sequence(std::string_view(pending_.data(), pending_size_), 0);
            if (pending_size_ < length) {
                return sequence.length_ == pending_size_;
            }
            if (!sequence.well_formed_) {
                return false;
            }
            pending_size_ = 0;
        }
        while (index < fragment.size()) {
            const auto lead = static_cast<unsigned char>(fragment[index]);
            if (lead < 0x80) {
                ++index;
                continue;
            }
            const auto sequence = scan_utf8_sequence(fragment, index);
            if (sequence.well_formed_) {
                index += sequence.length_;
                continue;
            }
            // A valid lead and continuation bytes cut off by the fragment end.
            if (index + sequence.length_ != fragment.size() || lead < 0xC2 || lead > 0xF4) {
                return false;
            }
            for (; index < fragment.size(); ++index) {
                pending_[pending_size_++] = fragment[index];
            }
        }
        return true;
    }

    [[nodiscard]] bool complete() const noexcept {
        return pending_size_ == 0;
    }

private:
    std::array<char, 4> pending_{};
    std::size_t pending_size_{0};
};

// The data message being submitted as fragments.
struct websocket_outbound_fragments final {
    websocket_opcode opcode_;
    // RSV1 went out on the first frame: every fragment continues one
    // permessage-deflate stream.
    bool compressed_;
    std::size_t size_;
    websocket_utf8_fragments utf8_;
};

}  // namespace

class websocket_connection::impl_type final {
public:
    enum class close_phase_type : std::uint8_t {
        open,
        // A locally initiated Close is at the tail of pending output. Once
        // flushed, the connection keeps receiving until the peer Close.
        local_close_queued,
        awaiting_peer_close,
        // A peer Close was received (or the connection failed) and the final
        // queued output still needs to be flushed before transport end. It may
        // be a Pong queued for an earlier Ping, not another Close.
        final_output_queued,
        // Client only (RFC 6455 section 7.1.1): the close handshake is complete
        // and the final output is still queued; once flushed, the server is
        // expected to close the transport first.
        handshake_output_queued,
        awaiting_peer_eof,
        transport_end_ready,
        closed,
    };

    // A null `input` gives the connection an input buffer of its own.
    impl_type(http_io_buffer* input, websocket_connection_options options)
        : input_(input != nullptr ? input : &owned_input_.emplace()),
          max_buffered_input_bytes_(options.max_buffered_input_bytes_),
          message_limit_(options.message_limit_),
          receive_message_limit_(options.receive_message_limit_.read_ceiling() < options.message_limit_.read_ceiling()
                                     ? options.receive_message_limit_
                                     : options.message_limit_),
          role_(options.role_),
          mask_key_generator_(options.mask_key_generator_),
          mask_key_context_(options.mask_key_context_) {
        initialize_codecs(options);
    }

    void initialize_codecs(const websocket_connection_options& options) {
        const auto& compression = options.compression_;
        if (max_buffered_input_bytes_ == 0) {
            throw std::invalid_argument("WebSocket input buffer limit must be greater than zero");
        }
        if (role_ != websocket_connection_role::server && role_ != websocket_connection_role::client) {
            throw std::invalid_argument("invalid WebSocket connection role");
        }
        if (compression.server_max_window_bits_.value_or(15) < 8 ||
            compression.server_max_window_bits_.value_or(15) > 15 ||
            compression.client_max_window_bits_.value_or(15) < 8 ||
            compression.client_max_window_bits_.value_or(15) > 15) {
            throw std::invalid_argument("invalid WebSocket compression window");
        }
        if (role_ == websocket_connection_role::client && mask_key_generator_ == nullptr) {
            throw std::invalid_argument("WebSocket client connection requires a mask key generator");
        }
        validate_websocket_deflater_config(options.deflater_);
        if (compression.enabled_) {
            const bool server = role_ == websocket_connection_role::server;
            const bool send_takeover =
                !(server ? compression.server_no_context_takeover_ : compression.client_no_context_takeover_);
            const bool receive_takeover =
                !(server ? compression.client_no_context_takeover_ : compression.server_no_context_takeover_);
            const int send_window =
                (server ? compression.server_max_window_bits_ : compression.client_max_window_bits_).value_or(15);
            const int receive_window =
                (server ? compression.client_max_window_bits_ : compression.server_max_window_bits_).value_or(15);
            deflate_.emplace(options.shared_deflate_, options.deflater_,
                send_takeover, receive_takeover, send_window, receive_window);
        }
    }

    [[nodiscard]] std::size_t pending_input_bytes() const noexcept {
        return input_->size() - pending_consume_;
    }

    void release_consumed_input() noexcept {
        // Keep the storage: the caller may append bytes that alias the released frame.
        input_->consume(std::exchange(pending_consume_, 0));
    }

    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept {
        switch (close_phase_) {
            case close_phase_type::open:
                return websocket_liveness_mode::open;
            case close_phase_type::local_close_queued:
            case close_phase_type::awaiting_peer_close:
                return websocket_liveness_mode::awaiting_peer_close;
            case close_phase_type::handshake_output_queued:
            case close_phase_type::awaiting_peer_eof:
                return websocket_liveness_mode::awaiting_peer_eof;
            case close_phase_type::final_output_queued:
            case close_phase_type::transport_end_ready:
            case close_phase_type::closed:
                return websocket_liveness_mode::inactive;
        }
        return websocket_liveness_mode::inactive;
    }

    void append_frame(websocket_opcode opcode, std::string_view payload_value, bool rsv1 = false,
        websocket_payload_storage storage = websocket_payload_storage::copy,
        websocket_fragment_position position = websocket_fragment_position::whole) {
        std::optional<std::string> owned_payload;
        if (detail::http_text_overlaps(payload_value, out_buffer_.view())) {
            // A plan may be its own append source. Snapshot it before prepare()
            // can move storage, and never borrow the temporary snapshot.
            payload_value = owned_payload.emplace(payload_value);
            storage = websocket_payload_storage::copy;
        }
        detail::websocket_frame_header_type header;
        const bool masked = role_ == websocket_connection_role::client;
        websocket_mask_key_type mask{};
        if (masked && !mask_key_generator_(mask_key_context_, mask)) {
            throw std::runtime_error("failed to generate WebSocket client mask key");
        }
        const bool borrow = storage == websocket_payload_storage::borrow && !masked;
        const bool continuation =
            position == websocket_fragment_position::middle || position == websocket_fragment_position::last;
        const bool final = position == websocket_fragment_position::whole || position == websocket_fragment_position::last;
        const auto header_size = detail::encode_websocket_frame_header(header,
            continuation ? std::uint8_t{0} : static_cast<std::uint8_t>(opcode), final, payload_value.size(), rsv1, masked);
        const auto mask_size = masked ? mask.size() : std::size_t{0};
        std::size_t additional_size = 0;
        for (const auto size : {borrowed_output_.size(), header_size, mask_size, borrow ? 0 : payload_value.size()}) {
            if (size > std::numeric_limits<std::size_t>::max() - additional_size) {
                throw std::length_error("WebSocket output frame size overflow");
            }
            additional_size += size;
        }
        // Prepare every byte before publishing anything. The appends below
        // then fit without allocation, so a failure cannot orphan a wire header.
        (void)out_buffer_.prepare(additional_size);
        // An earlier borrowed payload precedes this frame on the wire.
        out_buffer_.append(borrowed_output_);
        borrowed_output_ = {};
        out_buffer_.append(std::string_view(header.data(), header_size));
        if (masked) {
            out_buffer_.append(std::string_view(mask.data(), mask.size()));
            const auto payload_start = out_buffer_.size();
            out_buffer_.append(payload_value);
            detail::decode_masked_websocket_payload(
                out_buffer_.data() + payload_start, payload_value.size(), mask.data());
        } else if (borrow) {
            borrowed_output_ = payload_value;
        } else {
            out_buffer_.append(payload_value);
        }
    }

    void fail(std::uint16_t code) {
        if (close_phase_ == close_phase_type::open) {
            // Every protocol failure code is a valid Close code, so encoding succeeds.
            const auto payload_value = detail::encode_websocket_close_payload(websocket_close_code::from_value(code), {});
            append_frame(websocket_opcode::close, payload_value.encoded()->bytes());
            close_phase_ = close_phase_type::final_output_queued;
            return;
        }
        if (close_phase_ == close_phase_type::local_close_queued) {
            close_phase_ = close_phase_type::final_output_queued;
        } else if (close_phase_ == close_phase_type::awaiting_peer_close) {
            close_phase_ = !out_buffer_.empty() || !borrowed_output_.empty()
                               ? close_phase_type::final_output_queued
                               : close_phase_type::transport_end_ready;
        }
    }

    // Both Close frames have been exchanged once the queued output (which may
    // end with a Close echo) is flushed. A server then ends the transport; a
    // client waits for the server to do so.
    [[nodiscard]] close_phase_type handshake_complete_phase() const noexcept {
        const bool output_queued = !out_buffer_.empty() || !borrowed_output_.empty();
        if (role_ == websocket_connection_role::client) {
            return output_queued ? close_phase_type::handshake_output_queued : close_phase_type::awaiting_peer_eof;
        }
        return output_queued ? close_phase_type::final_output_queued : close_phase_type::transport_end_ready;
    }

    void receive_peer_close() noexcept {
        if (close_phase_ == close_phase_type::local_close_queued ||
            close_phase_ == close_phase_type::awaiting_peer_close) {
            close_phase_ = handshake_complete_phase();
        }
    }

    void discard_input() noexcept {
        pending_consume_ = 0;
        input_->consume(input_->size());
    }

    void record_termination(websocket_termination cause) noexcept {
        if (termination_ == websocket_termination::none) {
            termination_ = cause;
        }
    }

    // The one submission behind both submit_frame() payload forms.
    [[nodiscard]] websocket_frame_submit_status submit_frame(websocket_opcode opcode, std::string_view payload_value,
        websocket_frame_options options, websocket_payload_storage storage) {
        if (close_phase_ != close_phase_type::open) {
            return websocket_frame_submit_status::not_open;
        }
        if (opcode == websocket_opcode::ping || opcode == websocket_opcode::pong) {
            // Control frames are never fragmented (RFC 6455 section 5.5).
            if (!options.final_) {
                return websocket_frame_submit_status::invalid_opcode;
            }
            if (payload_value.size() > 125) {
                return websocket_frame_submit_status::control_frame_too_large;
            }
            append_frame(opcode, payload_value, false, storage);
            return websocket_frame_submit_status::accepted;
        }
        if (opcode != websocket_opcode::text && opcode != websocket_opcode::binary) {
            return websocket_frame_submit_status::invalid_opcode;
        }
        if (outbound_fragments_.has_value() && outbound_fragments_->opcode_ != opcode) {
            return websocket_frame_submit_status::message_in_progress;
        }
        const bool first = !outbound_fragments_.has_value();
        const bool final = options.final_;
        const std::size_t uncompressed_size = payload_value.size();
        const std::size_t sent = first ? 0 : outbound_fragments_->size_;
        if (message_limit_.addition_exceeds(sent, uncompressed_size)) {
            return websocket_frame_submit_status::message_too_large;
        }
        auto utf8 = first ? websocket_utf8_fragments{} : outbound_fragments_->utf8_;
        if (opcode == websocket_opcode::text && (!utf8.append(payload_value) || (final && !utf8.complete()))) {
            return websocket_frame_submit_status::invalid_text_payload;
        }
        const bool compression_attempted =
            first ? options.compress_ && deflate_.has_value() : outbound_fragments_->compressed_;
        bool compressed = compression_attempted;
        const auto position = first ? (final ? websocket_fragment_position::whole : websocket_fragment_position::first)
                                    : (final ? websocket_fragment_position::last : websocket_fragment_position::middle);
        try {
            if (compressed) {
                outbound_deflated_.clear();
                if (position == websocket_fragment_position::whole) {
                    // A whole message is sent uncompressed unless deflate shrinks it.
                    if (deflate_->compress(payload_value, outbound_deflated_) &&
                        outbound_deflated_.size() < payload_value.size()) {
                        payload_value = outbound_deflated_;
                    } else {
                        deflate_->discard_compression();
                        compressed = false;
                    }
                } else if (deflate_->compress_fragment(payload_value, first, final, outbound_deflated_)) {
                    payload_value = outbound_deflated_;
                } else if (first) {
                    // Nothing went out yet: send this message uncompressed.
                    deflate_->discard_compression();
                    compressed = false;
                } else {
                    throw std::runtime_error("failed to compress a WebSocket message fragment");
                }
            }
            // A compressed payload lives in outbound_deflated_, which the next
            // message reuses, so only the caller's own bytes can be borrowed.
            append_frame(opcode, payload_value, first && compressed,
                compressed ? websocket_payload_storage::copy : storage, position);
        } catch (...) {
            if (compression_attempted) {
                http_clear_string_retaining_small(outbound_deflated_, websocket_retained_buffer_bytes);
            }
            if (compressed) {
                // The peer never received a first frame's trial, so a later
                // message must not refer to its dictionary. Earlier fragments
                // already advanced the peer's inflater, so a later fragment
                // cannot be retried; nor can the first one once resetting its
                // trial fails.
                bool reusable = first;
                if (first) {
                    try {
                        deflate_->discard_compression();
                    } catch (...) {
                        reusable = false;
                    }
                }
                if (!reusable) {
                    close_phase_ = close_phase_type::transport_end_ready;
                    record_termination(websocket_termination::aborted);
                }
            }
            throw;
        }
        if (compression_attempted) {
            // The frame owns a copy now; keep only a bounded scratch.
            http_clear_string_retaining_small(outbound_deflated_, websocket_retained_buffer_bytes);
        }
        if (final) {
            outbound_fragments_.reset();
        } else if (first) {
            outbound_fragments_.emplace(websocket_outbound_fragments{opcode, compressed, uncompressed_size, utf8});
        } else {
            outbound_fragments_->size_ = sent + uncompressed_size;
            outbound_fragments_->utf8_ = utf8;
        }
        return websocket_frame_submit_status::accepted;
    }

    std::optional<http_io_buffer> owned_input_;
    http_io_buffer* input_;
    const std::size_t max_buffered_input_bytes_;
    protocol_byte_limit message_limit_;
    protocol_byte_limit receive_message_limit_;
    // Bytes of the last returned frame, consumed by the next next_event().
    std::size_t pending_consume_{0};
    std::size_t needed_input_bytes_{0};

    http_io_buffer out_buffer_;
    // An unmasked, uncompressed payload that follows out_buffer_ on the wire
    // without being copied into it. Only the newest frame can borrow; queuing
    // more output first appends it to out_buffer_.
    std::string_view borrowed_output_;

    detail::websocket_inbound_assembler assembler_;

    std::optional<detail::websocket_deflate> deflate_;
    std::string inbound_inflated_;
    std::string outbound_deflated_;
    std::optional<websocket_outbound_fragments> outbound_fragments_;

    websocket_connection_role role_{websocket_connection_role::server};
    websocket_mask_key_generator_type mask_key_generator_{nullptr};
    void* mask_key_context_{nullptr};

    close_phase_type close_phase_{close_phase_type::open};
    websocket_termination termination_{websocket_termination::none};
    std::uint64_t bytes_received_{0};
    std::uint64_t bytes_sent_{0};
};

websocket_connection::websocket_connection(websocket_connection_options options)
    : impl_(std::make_unique<impl_type>(nullptr, options)) {}

websocket_connection::websocket_connection(http_io_buffer& input, websocket_connection_options options)
    : impl_(std::make_unique<impl_type>(&input, options)) {}

websocket_connection::~websocket_connection() = default;
websocket_connection::websocket_connection(websocket_connection&&) noexcept = default;
websocket_connection& websocket_connection::operator=(websocket_connection&&) noexcept = default;

websocket_feed_status websocket_connection::feed(std::string_view input) {
    const auto liveness = impl_->liveness_mode();
    if (liveness == websocket_liveness_mode::inactive) {
        return websocket_feed_status::inactive;
    }
    if (liveness == websocket_liveness_mode::awaiting_peer_eof) {
        // Bytes after the close handshake carry nothing; the client keeps
        // reading only to observe the server's EOF.
        return websocket_feed_status::accepted;
    }
    if (impl_->input_->size() > impl_->max_buffered_input_bytes_ ||
        input.size() > impl_->max_buffered_input_bytes_ - impl_->input_->size()) {
        if (impl_->pending_input_bytes() > impl_->max_buffered_input_bytes_ ||
            input.size() > impl_->max_buffered_input_bytes_ - impl_->pending_input_bytes()) {
            return websocket_feed_status::backpressured;
        }
        // The bound covers bytes awaiting next_event(), not the frame already
        // returned; otherwise a frame that fits could never arrive. append()
        // copies an `input` aliasing the buffer before it moves any byte.
        impl_->release_consumed_input();
    }
    impl_->input_->append(input);
    return websocket_feed_status::accepted;
}

std::optional<websocket_event> websocket_connection::next_event() & {
    using close_phase_type = impl_type::close_phase_type;
    try {
        impl_->assembler_.release_completed();
        // The last delivered message expires now. Its size is peer-controlled up
        // to the message limit, so only a bounded buffer stays for the next one.
        http_clear_string_retaining_small(
            impl_->inbound_inflated_, websocket_retained_buffer_bytes);
        impl_->needed_input_bytes_ = 0;
        if (impl_->close_phase_ == close_phase_type::final_output_queued ||
            impl_->close_phase_ == close_phase_type::transport_end_ready ||
            impl_->close_phase_ == close_phase_type::closed) {
            return websocket_event::make_transport_end();
        }
        if (impl_->close_phase_ == close_phase_type::handshake_output_queued) {
            // Queued output may still borrow input bytes: drop them once flushed.
            return std::nullopt;
        }
        if (impl_->close_phase_ == close_phase_type::awaiting_peer_eof) {
            impl_->discard_input();
            return std::nullopt;
        }

        const auto protocol_failure_event = [this](detail::websocket_protocol_failure failure) {
            const auto close_code = detail::websocket_protocol_failure_close_code(failure);
            impl_->fail(close_code);
            impl_->record_termination(websocket_failure_termination(failure));
            return websocket_event::protocol_error(close_code);
        };

        for (;;) {
            const auto read = detail::websocket_try_read_frame(*impl_->input_, impl_->pending_consume_,
                impl_->receive_message_limit_, impl_->deflate_.has_value(),
                impl_->role_ == websocket_connection_role::server);
            if (const auto* need_input = read.need_input()) {
                impl_->needed_input_bytes_ = need_input->missing_bytes();
                return std::nullopt;
            }
            if (const auto* failure = read.failure()) {
                return protocol_failure_event(failure->error());
            }

            add_saturating(impl_->bytes_received_, impl_->pending_consume_);
            const auto& frame = *read.frame();
            const auto inbound = impl_->assembler_.accept(frame, impl_->receive_message_limit_);
            if (const auto* failure = inbound.failure()) {
                return protocol_failure_event(failure->error());
            }
            if (inbound.continue_reading() != nullptr) {
                continue;
            }
            if (const auto* control = inbound.control_frame()) {
                const auto payload_value = control->payload();
                if (control->opcode() == websocket_opcode::ping) {
                    // RFC 6455 requires Pong until a peer Close has arrived. A Pong
                    // is a control frame, so it remains legal while a locally
                    // initiated Close waits for its peer response.
                    impl_->append_frame(websocket_opcode::pong, payload_value);
                    return websocket_event::ping(payload_value);
                }
                if (control->opcode() == websocket_opcode::pong) {
                    return websocket_event::pong(payload_value);
                }
                // Close: the frame reader admits no other control opcode.
                std::uint16_t code = 1005;
                if (payload_value.size() >= 2) {
                    code = detail::read_websocket_uint16(payload_value.data());
                }
                const auto reason = payload_value.size() > 2 ? payload_value.substr(2) : std::string_view{};
                if (impl_->close_phase_ == close_phase_type::open) {
                    impl_->append_frame(websocket_opcode::close, payload_value);
                    impl_->close_phase_ = impl_->handshake_complete_phase();
                    impl_->record_termination(websocket_termination::peer_close);
                } else {
                    impl_->receive_peer_close();
                    impl_->record_termination(websocket_termination::local_close);
                }
                return websocket_event::close(code, reason);
            }

            const auto& inbound_message = *inbound.message();
            const auto& message = inbound_message.message();
            if (inbound_message.content_encoding() == detail::websocket_inbound_content_encoding::identity) {
                if (impl_->close_phase_ != close_phase_type::open) {
                    impl_->assembler_.release_completed();
                    continue;
                }
                return websocket_event::message(message.opcode(), message.payload());
            }

            // decompress() only appends, so the buffer must be emptied per MESSAGE, not
            // per next_event(): one next_event() drains several frames, and a message suppressed
            // during the closing handshake (below) returns via `continue` with its bytes
            // still here. Inheriting them would make the next message's UTF-8 check read
            // the concatenation, and would charge its decompression-bomb limit for both.
            impl_->inbound_inflated_.clear();
            // Only a negotiated deflate admits RSV1, so a compressed message implies deflate_.
            const auto inflate_result = impl_->deflate_->decompress(
                message.payload(), impl_->inbound_inflated_, impl_->receive_message_limit_);
            if (inflate_result == websocket_inflate_result::too_large) {
                return protocol_failure_event(detail::websocket_protocol_failure::message_too_large);
            }
            if (inflate_result != websocket_inflate_result::ok) {
                return protocol_failure_event(detail::websocket_protocol_failure::protocol_error);
            }
            const std::string_view view = impl_->inbound_inflated_;
            if (message.opcode() == websocket_opcode::text && !is_valid_utf8(view)) {
                return protocol_failure_event(detail::websocket_protocol_failure::invalid_payload_data);
            }
            if (impl_->close_phase_ != close_phase_type::open) {
                impl_->assembler_.release_completed();
                http_clear_string_retaining_small(
                    impl_->inbound_inflated_, websocket_retained_buffer_bytes);
                continue;
            }
            return websocket_event::message(message.opcode(), view);
        }
    } catch (...) {
        // Frame reading unmasks in place and advances the input cursor. If any
        // later assembler/deflate/output operation fails, retrying that frame is
        // no longer well-defined. The transport has not ended yet, so the plan
        // and abort() must still tell the driver to end it, as after EOF.
        if (impl_->close_phase_ != close_phase_type::closed) {
            impl_->close_phase_ = close_phase_type::transport_end_ready;
        }
        impl_->record_termination(websocket_termination::aborted);
        throw;
    }
}

std::size_t websocket_connection::needed_input_bytes() const noexcept {
    return impl_->needed_input_bytes_;
}

websocket_output_plan websocket_connection::output_plan() const& noexcept {
    using close_phase_type = impl_type::close_phase_type;
    // EOF/abort may race an async transport write. Keep the backing allocation
    // untouched until destruction, but make discarded bytes unreachable from the
    // protocol driver once transport termination has become authoritative.
    if (impl_->close_phase_ == close_phase_type::transport_end_ready ||
        impl_->close_phase_ == close_phase_type::closed) {
        return websocket_output_plan({}, {},
            impl_->close_phase_ == close_phase_type::transport_end_ready
                ? websocket_transport_disposition::end_transport
                : websocket_transport_disposition::keep_open);
    }
    const auto bytes_value = impl_->out_buffer_.view();
    const auto disposition = impl_->close_phase_ == close_phase_type::final_output_queued
                                 ? websocket_transport_disposition::end_transport
                                 : websocket_transport_disposition::keep_open;
    return websocket_output_plan(bytes_value, impl_->borrowed_output_, disposition);
}

websocket_output_consume_status websocket_connection::consume_output(std::size_t bytes_value) noexcept {
    using close_phase_type = impl_type::close_phase_type;
    // EOF/abort makes unsent bytes unreachable without clearing their storage:
    // an async write may still borrow it. A zero-byte transport-end write has
    // no output left to consume, regardless of those discarded backing bytes.
    if (bytes_value == 0 && (impl_->close_phase_ == close_phase_type::transport_end_ready ||
                                impl_->close_phase_ == close_phase_type::closed)) {
        return websocket_output_consume_status::drained;
    }
    const auto owned = impl_->out_buffer_.size();
    if (bytes_value > owned && bytes_value - owned > impl_->borrowed_output_.size()) {
        return websocket_output_consume_status::out_of_range;
    }
    add_saturating(impl_->bytes_sent_, bytes_value);
    if (bytes_value < owned) {
        impl_->out_buffer_.consume(bytes_value);
        return websocket_output_consume_status::pending;
    }
    // The transport has written every owned byte, so a large frame's storage
    // can go back to the resource instead of staying with the connection.
    impl_->out_buffer_.clear_retaining_small(websocket_retained_buffer_bytes);
    if (bytes_value - owned < impl_->borrowed_output_.size()) {
        impl_->borrowed_output_.remove_prefix(bytes_value - owned);
        return websocket_output_consume_status::pending;
    }
    impl_->borrowed_output_ = {};
    if (impl_->close_phase_ == close_phase_type::local_close_queued) {
        impl_->close_phase_ = close_phase_type::awaiting_peer_close;
    } else if (impl_->close_phase_ == close_phase_type::final_output_queued) {
        impl_->close_phase_ = close_phase_type::transport_end_ready;
    } else if (impl_->close_phase_ == close_phase_type::handshake_output_queued) {
        impl_->close_phase_ = close_phase_type::awaiting_peer_eof;
    }
    return websocket_output_consume_status::drained;
}

void websocket_connection::commit_transport_end() noexcept {
    if (impl_->close_phase_ == impl_type::close_phase_type::transport_end_ready) {
        impl_->close_phase_ = impl_type::close_phase_type::closed;
    }
}

void websocket_connection::notify_transport_eof() noexcept {
    if (impl_->close_phase_ != impl_type::close_phase_type::closed) {
        impl_->close_phase_ = impl_type::close_phase_type::transport_end_ready;
    }
    impl_->record_termination(websocket_termination::eof);
}

websocket_abort_disposition websocket_connection::abort(websocket_termination cause) noexcept {
    impl_->record_termination(cause == websocket_termination::none ? websocket_termination::aborted : cause);
    if (impl_->close_phase_ == impl_type::close_phase_type::closed) {
        return websocket_abort_disposition::no_transport_action;
    }
    impl_->close_phase_ = impl_type::close_phase_type::closed;
    return websocket_abort_disposition::abort_transport;
}

websocket_liveness_mode websocket_connection::liveness_mode() const noexcept {
    return impl_->liveness_mode();
}

websocket_termination websocket_connection::termination() const noexcept {
    return impl_->termination_;
}

std::uint64_t websocket_connection::bytes_received() const noexcept {
    return impl_->bytes_received_;
}

std::uint64_t websocket_connection::bytes_sent() const noexcept {
    return impl_->bytes_sent_;
}

websocket_frame_submit_status websocket_connection::submit_frame(
    websocket_opcode opcode, std::string_view payload_value, websocket_frame_options options) {
    return impl_->submit_frame(opcode, payload_value, options, websocket_payload_storage::copy);
}

websocket_frame_submit_status websocket_connection::submit_frame(
    websocket_opcode opcode, websocket_borrowed_payload payload_value, websocket_frame_options options) {
    return impl_->submit_frame(opcode, payload_value.bytes(), options, websocket_payload_storage::borrow);
}

websocket_close_submit_status websocket_connection::submit_close(
    websocket_close_code code, std::string_view reason) {
    using close_phase_type = impl_type::close_phase_type;
    if (impl_->close_phase_ == close_phase_type::closed) {
        return websocket_close_submit_status::closed;
    }
    if (impl_->close_phase_ != close_phase_type::open) {
        return websocket_close_submit_status::already_closing;
    }
    // RFC 6455 §7.4.1 reserves 1010 for a client reporting extensions that
    // were absent from the server handshake. This core emits server frames;
    // a server must reject that mismatch during the opening handshake rather
    // than initiate a Close frame with the client-only status code.
    if (impl_->role_ == websocket_connection_role::server && code.value() == 1010) {
        return websocket_close_submit_status::invalid_code;
    }
    const auto payload_value = detail::encode_websocket_close_payload(code, reason);
    if (const auto* failure = payload_value.failure()) {
        switch (failure->error()) {
            case detail::websocket_close_payload_encode_error::invalid_reason:
                return websocket_close_submit_status::invalid_reason;
            case detail::websocket_close_payload_encode_error::reason_too_large:
                return websocket_close_submit_status::reason_too_large;
        }
    }
    impl_->append_frame(websocket_opcode::close, payload_value.encoded()->bytes());
    impl_->close_phase_ = close_phase_type::local_close_queued;
    return websocket_close_submit_status::accepted;
}

}  // namespace ruvia

namespace ruvia {

websocket_deflate_codecs::~websocket_deflate_codecs() {
    for (auto* codec : deflaters_) {
        delete codec;
    }
    delete inflater_;
}

bool websocket_deflate_codecs::deflate_message(
    std::string_view payload, std::string& out, int compression_level, int window_bits) {
    if (window_bits < min_window_bits || window_bits > max_window_bits) {
        throw std::invalid_argument("invalid WebSocket DEFLATE window");
    }
    if (compression_level < 0 || compression_level >= compression_levels) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    auto& slot = deflaters_[static_cast<std::size_t>(
        (window_bits - min_window_bits) * compression_levels + compression_level)];
    auto* codec = detail::acquire_codec(slot, compression_level,
        detail::websocket_deflate_zlib_window_bits(window_bits), detail::zlib_default_mem_level,
        detail::websocket_deflate_zlib_strategy(window_bits));
    if (codec == nullptr) {
        return false;
    }
    const detail::codec_reset_guard reset_guard(slot);
    return detail::websocket_deflate_message(*codec, payload, out);
}

websocket_inflate_result websocket_deflate_codecs::inflate_message(
    std::string_view payload, std::string& out, protocol_byte_limit message_limit) {
    auto* codec = detail::acquire_codec(inflater_, -max_window_bits);
    if (codec == nullptr) {
        return websocket_inflate_result::error;
    }
    const detail::codec_reset_guard reset_guard(inflater_);
    return detail::websocket_inflate_message(*codec, payload, out, message_limit);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] std::vector<http_header> copy_websocket_response_headers(
    std::span<const http_header_view> headers, std::string_view subprotocol,
    std::string_view extension) {
    // Account for the whole emitted head: the fixed 101 prefix, the accept
    // key line, the negotiated and application fields, and the terminating
    // empty line.
    http_field_section_budget size;
    (void)size.add(websocket_switching_protocols_head_prefix.size() + websocket_accept_key_type{}.size() +
                   std::string_view("\r\n").size());
    (void)size.add(std::string_view("\r\n").size());
    if (!subprotocol.empty() && !size.add_field("sec-websocket-protocol", subprotocol)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    if (!extension.empty() && !size.add_field("sec-websocket-extensions", extension)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    static constexpr std::string_view sec_websocket_prefix = "sec-websocket-";
    for (const auto& header : headers) {
        const auto name = header.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid WebSocket handshake response header");
        }
        // Every Sec-WebSocket-* name is handshake-owned, including ones this
        // implementation does not know yet.
        const auto kind = classify_http_field(name);
        const bool is_sec_websocket =
            name.size() >= sec_websocket_prefix.size() &&
            http_ascii_equals_ignore_case(
                name.substr(0, sec_websocket_prefix.size()), sec_websocket_prefix);
        if ((kind != http_field_kind::host &&
                kind != http_field_kind::expect &&
                is_protocol_managed_request_field(kind, name)) ||
            is_sec_websocket) {
            throw std::invalid_argument("WebSocket handshake controls this response header");
        }
        if (!size.add_field(name, header.value())) {
            throw std::length_error("WebSocket handshake response headers are too large");
        }
    }
    std::vector<http_header> result;
    result.reserve(headers.size());
    for (const auto& header : headers) {
        result.push_back(
            http_header::copy_with_lowercase_name(header.name(), header.value()));
    }
    return result;
}

void skip_websocket_extension_ows(std::string_view value, std::size_t& cursor_value) noexcept {
    while (cursor_value < value.size() && (value[cursor_value] == ' ' || value[cursor_value] == '\t')) {
        ++cursor_value;
    }
}

[[nodiscard]] bool consume_websocket_extension_token(
    std::string_view value, std::size_t& cursor_value) noexcept {
    const auto start = cursor_value;
    while (cursor_value < value.size() && is_http_token_char(static_cast<unsigned char>(value[cursor_value]))) {
        ++cursor_value;
    }
    return cursor_value != start;
}

// RFC 6455 section 4.3 narrows quoted-string extension values: after
// quoted-pair unescaping, the result still has to be a nonempty token.
[[nodiscard]] bool consume_websocket_extension_quoted_token(
    std::string_view value, std::size_t& cursor_value) noexcept {
    http_quoted_string_cursor quoted(value.substr(cursor_value));
    bool decoded_any = false;
    unsigned char byte = 0;
    while (quoted.next(byte)) {
        if (!is_http_token_char(byte)) {
            return false;
        }
        decoded_any = true;
    }
    if (!quoted.closed()) {
        return false;
    }
    cursor_value += quoted.consumed();
    return decoded_any;
}

}  // namespace

bool append_websocket_extension_list(
    std::string_view value, bool& has_extension) noexcept {
    std::size_t cursor_value = 0;
    while (true) {
        skip_websocket_extension_ows(value, cursor_value);
        // RFC 2616's #rule permits null comma-list elements, but 1#extension
        // still requires at least one real extension across the logical field.
        while (cursor_value < value.size() && value[cursor_value] == ',') {
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
        }
        if (cursor_value == value.size()) {
            return true;
        }
        if (!consume_websocket_extension_token(value, cursor_value)) {
            return false;
        }
        has_extension = true;

        while (true) {
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size()) {
                return true;
            }
            if (value[cursor_value] == ',') {
                ++cursor_value;
                break;
            }
            if (value[cursor_value] != ';') {
                return false;
            }
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
            if (!consume_websocket_extension_token(value, cursor_value)) {
                return false;
            }
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size() || value[cursor_value] != '=') {
                continue;
            }
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size()) {
                return false;
            }
            if (value[cursor_value] == '"') {
                if (!consume_websocket_extension_quoted_token(value, cursor_value)) {
                    return false;
                }
            } else if (!consume_websocket_extension_token(value, cursor_value)) {
                return false;
            }
        }
    }
}

std::string_view choose_websocket_subprotocol(
    const websocket_subprotocol_set& offered,
    std::span<const std::string_view> supported) noexcept {
    websocket_subprotocol_set configured;
    for (const auto protocol : supported) {
        if (!configured.append(protocol)) {
            return {};
        }
    }
    if (offered.empty()) {
        return {};
    }
    for (const auto protocol : supported) {
        if (offered.contains(protocol)) {
            return protocol;
        }
    }
    return {};
}

std::string_view choose_websocket_subprotocol(
    const http_request& request, std::span<const std::string_view> supported) noexcept {
    if (supported.empty()) {
        return {};
    }
    websocket_subprotocol_set offered;
    if (!http_request_access::visit_known_headers(request, http_field_kind::sec_websocket_protocol,
            [&offered](std::string_view value) noexcept { return offered.append_offered_list(value); })) {
        return {};
    }
    return choose_websocket_subprotocol(offered, supported);
}

bool websocket_request_header_validator::observe_header(
    http_field_kind kind, std::string_view value) noexcept {
    if (kind == http_field_kind::sec_websocket_version) {
        version_ = value;
        if (version_present_) {
            valid_ = false;
        }
        version_present_ = true;
    } else if (kind == http_field_kind::sec_websocket_protocol) {
        protocols_present_ = true;
        if (valid_ && !(role_ == http_field_list_role::recipient ? protocols_.append_offered_list(value)
                                                                 : protocols_.append_list(value))) {
            valid_ = false;
        }
    } else if (kind == http_field_kind::sec_websocket_extensions) {
        extensions_present_ = true;
        if (valid_ && !append_websocket_extension_list(value, has_extension_)) {
            valid_ = false;
        }
    }
    return valid_;
}

bool websocket_request_header_validator::offers_valid() const noexcept {
    return valid_ && version_present_ && (!protocols_present_ || !protocols_.empty()) &&
           (!extensions_present_ || has_extension_);
}

bool websocket_request_header_validator::client_offers_valid() const noexcept {
    return offers_valid() && version_ == "13";
}

websocket_handshake_validation_result websocket_request_header_validator::finish(
    websocket_handshake_validation_options options) const noexcept {
    if (!offers_valid()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    if (http_trim_ows(version_) != "13") {
        return websocket_handshake_validation_result::make_unsupported_version();
    }
    if (options.require_subprotocol_ &&
        choose_websocket_subprotocol(protocols_, options.supported_subprotocols_).empty()) {
        return websocket_handshake_validation_result::make_unsupported_subprotocol();
    }
    return websocket_handshake_validation_result::make_accepted();
}

websocket_server_negotiation::websocket_server_negotiation(std::string_view subprotocol,
    websocket_compression compression, std::span<const http_header_view> response_headers_value)
    : subprotocol_(subprotocol),
      compression_(compression),
      extensions_(compression),
      response_headers_(copy_websocket_response_headers(
          response_headers_value, subprotocol, extensions_.view())) {}

websocket_server_negotiation make_websocket_server_negotiation(
    const http_request& request, const websocket_server_handshake_options& options) {
    return websocket_server_negotiation(choose_websocket_subprotocol(request, options.supported_subprotocols_),
        websocket_negotiate_permessage_deflate(request, options.deflate_), options.response_headers_);
}

websocket_handshake_validation_result validate_extended_connect_websocket_headers(
    const http_request& request, websocket_handshake_validation_options options) noexcept {
    websocket_request_header_validator validator(http_field_list_role::recipient);
    const auto headers = request.headers();
    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (!validator.observe_header(http_request_access::header_kind(request, index), headers[index].value())) {
            return validator.finish(options);
        }
    }
    return validator.finish(options);
}

}  // namespace ruvia::detail

namespace ruvia {

websocket_server_handshake make_websocket_server_handshake(
    const http_request& request, websocket_server_handshake_options options) {
    detail::websocket_accept_key_type accept;
    detail::encode_websocket_accept(
        accept, detail::http_request_access::known_header(request, detail::http_field_kind::sec_websocket_key));
    auto negotiation = detail::make_websocket_server_negotiation(request, options);
    return websocket_server_handshake(accept, std::move(negotiation).take_subprotocol(),
        negotiation.compression(), std::move(negotiation).take_response_headers());
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

constexpr std::string_view websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::uint64_t max_sha1_message_bytes = (std::numeric_limits<std::uint64_t>::max)() / 8;

[[nodiscard]] std::array<std::uint8_t, 20> sha1(
    std::string_view first, std::string_view second) noexcept {
    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;
    std::array<std::uint8_t, 64> block{};
    const auto total_size = first.size() + second.size();
    const std::uint64_t total_bits = static_cast<std::uint64_t>(total_size) * 8U;
    std::size_t offset = 0;

    const auto byte_at = [first, second](std::size_t index) noexcept {
        return index < first.size() ? static_cast<std::uint8_t>(first[index])
                                    : static_cast<std::uint8_t>(second[index - first.size()]);
    };

    const auto process = [&](const std::array<std::uint8_t, 64>& data) noexcept {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(data[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        auto a = h0;
        auto b = h1;
        auto c = h2;
        auto d = h3;
        auto e = h4;
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const auto temp = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    };

    while (total_size - offset >= block.size()) {
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = byte_at(offset + i);
        }
        process(block);
        offset += block.size();
    }
    block.fill(0);
    const auto remaining = total_size - offset;
    for (std::size_t i = 0; i < remaining; ++i) {
        block[i] = byte_at(offset + i);
    }
    block[remaining] = 0x80;
    if (remaining >= 56) {
        process(block);
        block.fill(0);
    }
    for (std::size_t i = 0; i < 8; ++i) {
        block[63 - i] = static_cast<std::uint8_t>((total_bits >> (i * 8)) & 0xFF);
    }
    process(block);

    std::array<std::uint8_t, 20> digest{};
    const std::array words{h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < words.size(); ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((words[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(words[i] & 0xFF);
    }
    return digest;
}

}  // namespace

void encode_websocket_accept(websocket_accept_key_type& output, std::string_view key) {
    // Check the raw view before trimming: http_trim_ows() must inspect the last
    // byte, and callers of this detail helper are not otherwise required to
    // prove that a view's advertised size is backed by a full object. The
    // SHA-1 length field is measured in bits and is only 64 bits wide.
    if (key.size() > (std::numeric_limits<std::size_t>::max)() - websocket_guid.size() ||
        key.size() > max_sha1_message_bytes - websocket_guid.size()) {
        throw std::length_error("WebSocket accept input is too large");
    }
    key = http_trim_ows(key);
    const auto digest = sha1(key, websocket_guid);
    http_base64_encode(output.data(), std::span<const std::uint8_t>(digest));
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// A Sec-WebSocket-Key is the canonical padded base64 of a 16-byte nonce
// (RFC 6455 section 4.1), so exactly 24 characters.
[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> decode_websocket_key(
    std::string_view key) noexcept {
    key = http_trim_ows(key);
    std::array<std::uint8_t, 16> nonce{};
    if (key.size() != 24) {
        return std::nullopt;
    }
    const auto decoded =
        http_base64_decode(key, nonce, http_base64_charset::standard, http_base64_padding::required);
    if (!decoded || *decoded != nonce.size()) {
        return std::nullopt;
    }
    return nonce;
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

http_protocol_error websocket_handshake_failure::protocol_error() const noexcept {
    switch (kind_) {
        case kind_type::invalid_request:
            return http_protocol_error(http_status::bad_request, "invalid_websocket_handshake",
                "invalid WebSocket handshake");
        case kind_type::unsupported_version:
            // RFC 6455 section 4.4 requires Sec-websocket-Version and uses 400
            // in its example. 426 would instead require HTTP Upgrade fields.
            return http_protocol_error(http_status::bad_request, "unsupported_websocket_version",
                "unsupported WebSocket version");
        case kind_type::unsupported_subprotocol:
            return http_protocol_error(http_status::bad_request,
                "unsupported_websocket_subprotocol", "no supported WebSocket subprotocol offered");
    }
    return http_protocol_error(
        http_status::bad_request, "invalid_websocket_handshake", "invalid WebSocket handshake");
}

void websocket_handshake_failure::apply_required_response_headers(http_response& response) const {
    if (kind_ == kind_type::unsupported_version) {
        response.header_stable_view("Sec-WebSocket-Version", "13");
    }
}

websocket_handshake_validation_result validate_websocket_handshake(const http_request& request,
    const http1_request_body_plan& body_plan, websocket_handshake_validation_options options) noexcept {
    if (request.known_method() != http_known_method::get ||
        request.protocol_version() != http_protocol_version::http11 ||
        // RFC 6455 does not forbid Content-Length on the HTTP Upgrade request.
        // The parser-owned framing plan is the authoritative distinction:
        // Content-Length: 0 carries no content, while a positive length or
        // chunked coding still has bytes that must be consumed before the
        // connection can change protocols.
        body_plan.requires_consumption()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }

    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    detail::websocket_request_header_validator websocket_headers(detail::http_field_list_role::recipient);
    std::string_view key;
    std::size_t key_count = 0;
    bool websocket_upgrade = false;

    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = detail::http_request_access::header_kind(request, i);
        const auto& header_value = headers[i];
        if (kind == detail::http_field_kind::connection) {
            if (connection_options.parse_field(
                    header_value.value(), detail::http_connection_field_role::request_recipient) !=
                detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == detail::http_field_kind::upgrade) {
            if (upgrade_protocols.parse_field(header_value.value(), detail::http_field_list_role::recipient,
                    [&websocket_upgrade](const detail::http_upgrade_protocol& protocol) noexcept {
                        if (protocol.version_.empty() &&
                            http_ascii_equals_ignore_case(protocol.name_, "websocket")) {
                            websocket_upgrade = true;
                        }
                        return true;
                    }) != detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == detail::http_field_kind::sec_websocket_key) {
            key = header_value.value();
            ++key_count;
        } else if (!websocket_headers.observe_header(kind, header_value.value())) {
            return websocket_handshake_validation_result::make_invalid_request();
        }
    }

    if (!connection_options.upgrade() || !upgrade_protocols.has_protocol() || !websocket_upgrade ||
        key_count != 1 || !detail::decode_websocket_key(key).has_value()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return websocket_headers.finish(options);
}

}  // namespace ruvia

namespace ruvia {
namespace {
using error_type = websocket_client_handshake_error;
std::optional<websocket_compression> parse_response_extension(std::string_view text, const websocket_client_deflate_offer& deflate) {
    const auto& offer = deflate.compression_;
    if (!offer.enabled_) {
        return {};
    }
    const auto params = detail::parse_permessage_deflate_params(text);
    if (!params.has_value()) {
        return {};
    }
    // A response must give client_max_window_bits a value, and may only send it
    // when the offer included it (RFC 7692 section 7.1.2.2).
    if (params->client_max_window_bits_present_ &&
        (!params->client_max_window_bits_.has_value() ||
            (!deflate.offer_client_max_window_bits_ && !offer.client_max_window_bits_))) {
        return {};
    }
    websocket_compression result_value{.enabled_ = true,
        .server_no_context_takeover_ = params->server_no_context_takeover_,
        // The client's own takeover opt-out binds its deflater even when the server does not echo it.
        .client_no_context_takeover_ = offer.client_no_context_takeover_ || params->client_no_context_takeover_,
        .server_max_window_bits_ = params->server_max_window_bits_,
        .client_max_window_bits_ = params->client_max_window_bits_};
    if (offer.server_no_context_takeover_ && !result_value.server_no_context_takeover_) {
        return {};
    }
    if (offer.server_max_window_bits_ && (!result_value.server_max_window_bits_ || *result_value.server_max_window_bits_ > *offer.server_max_window_bits_)) {
        return {};
    }
    // An offered client window binds this client even when the response omits
    // it or selects a larger one (RFC 7692 section 7.1.2.2).
    if (offer.client_max_window_bits_ &&
        (!result_value.client_max_window_bits_ || *result_value.client_max_window_bits_ > *offer.client_max_window_bits_)) {
        result_value.client_max_window_bits_ = offer.client_max_window_bits_;
    }

    return result_value;
}
[[nodiscard]] bool subprotocol_offered(
    std::string_view offered_list, std::string_view selected) noexcept {
    if (selected.empty()) {
        return false;
    }
    return !detail::http_find_header_token(offered_list, [selected](std::string_view token) noexcept {
        return token == selected;
    }).empty();
}

std::variant<websocket_client_negotiation_result_view, error_type> validate(std::span<const http_header> fields_value,
    std::string_view offered_subprotocols, const websocket_client_deflate_offer& offer, bool extended) {
    websocket_client_negotiation_result_view result;
    std::size_t selected = 0;
    bool extensions_present = false;
    bool saw_extension = false;
    for (const auto& field : fields_value) {
        const auto kind = detail::classify_http_field(field.name());
        if (kind == detail::http_field_kind::sec_websocket_protocol) {
            ++selected;
            result.selected_subprotocol_ = http_trim_ows(field.value());
            if (selected > 1 || !subprotocol_offered(offered_subprotocols, result.selected_subprotocol_)) {
                return error_type::subprotocol;
            }
        } else if (kind == detail::http_field_kind::sec_websocket_extensions) {
            extensions_present = true;
            bool valid_line = true;
            http_visit_comma_separated_quoted_field_items(
                field.value(), [&](std::string_view item) noexcept {
                    if (item.empty()) {
                        return true;
                    }
                    saw_extension = true;
                    if (result.compression_.enabled_) {
                        valid_line = false;
                        return false;
                    }
                    auto negotiation = parse_response_extension(item, offer);
                    if (!negotiation) {
                        valid_line = false;
                        return false;
                    }
                    result.compression_ = *negotiation;
                    return true;
                });
            if (!valid_line) {
                return error_type::extensions;
            }
        } else if (extended && detail::is_forbidden_http_binary_response_field(field.name())) {
            // Connection-specific fields are malformed in HTTP/2 (RFC 9113
            // section 8.2.2). Sec-WebSocket-Accept is superseded by :protocol
            // and therefore not processed (RFC 8441 section 5).
            return error_type::forbidden_field;
        }
    }
    if (extensions_present && !saw_extension) {
        return error_type::extensions;
    }
    return result;
}

inline constexpr std::string_view version_header_name = "sec-websocket-version";
inline constexpr std::string_view version_storage = "sec-websocket-version13";
inline constexpr std::string_view protocol_header_name = "sec-websocket-protocol";
inline constexpr std::string_view extensions_header_name = "sec-websocket-extensions";
inline constexpr std::string_view client_window_bits_suffix = "; client_max_window_bits";

[[nodiscard]] const std::string_view* static_deflate_extension_storage(
    const websocket_client_deflate_offer& deflate) noexcept {
    const auto& offer = deflate.compression_;
    if (!offer.enabled_ || offer.server_max_window_bits_.has_value() || offer.client_max_window_bits_.has_value()) {
        return nullptr;
    }
    static constexpr std::array<std::string_view, 8> table{
        "sec-websocket-extensionspermessage-deflate",
        "sec-websocket-extensionspermessage-deflate; client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; client_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; client_no_context_takeover; "
        "client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_no_context_takeover; client_max_window_bits",
    };
    const std::size_t index = (offer.server_no_context_takeover_ ? 4U : 0U) |
                              (offer.client_no_context_takeover_ ? 2U : 0U) |
                              (deflate.offer_client_max_window_bits_ ? 1U : 0U);
    return &table[index];
}

// The validated request-field plan of one client configuration.
struct client_offer_plan final {
    // Bytes of the comma-joined Sec-WebSocket-Protocol value; zero without subprotocols.
    std::size_t subprotocol_bytes_{0};
    // Name-prefixed static storage of a parameterless deflate offer.
    const std::string_view* static_extension_{nullptr};
    // A deflate offer carrying window parameters.
    std::optional<detail::websocket_compression_extension> encoded_extension_;
    // The offer lists client_max_window_bits without a value after encoded_extension_.
    bool client_window_bits_suffix_{false};
};

[[nodiscard]] client_offer_plan make_client_offer_plan(const websocket_client_negotiation_config_view& config) {
    const auto& offer = config.deflate_.compression_;
    auto valid_window = [](std::optional<int> window_bits) {
        return !window_bits || (*window_bits >= 8 && *window_bits <= 15);
    };
    if (!valid_window(offer.server_max_window_bits_) || !valid_window(offer.client_max_window_bits_) ||
        (offer.client_max_window_bits_ && !config.deflate_.offer_client_max_window_bits_)) {
        throw std::invalid_argument("invalid WebSocket client deflate offer");
    }
    validate_websocket_deflater_config(config.deflate_.deflater_);
    client_offer_plan plan;
    websocket_subprotocol_set seen;
    for (const auto protocol_view : config.subprotocols_) {
        if (!seen.append(protocol_view)) {
            throw std::invalid_argument("invalid WebSocket subprotocol");
        }
        if (plan.subprotocol_bytes_ != 0) {
            if (plan.subprotocol_bytes_ > max_http_header_bytes - 2) {
                throw std::invalid_argument("WebSocket subprotocol field too large");
            }
            plan.subprotocol_bytes_ += 2;
        }
        if (protocol_view.size() > max_http_header_bytes - plan.subprotocol_bytes_) {
            throw std::invalid_argument("WebSocket subprotocol field too large");
        }
        plan.subprotocol_bytes_ += protocol_view.size();
    }
    // The generated Sec-WebSocket-* fields alone must fit the field section;
    // the handshake owner budgets the application headers it accepts.
    http_field_section_budget budget;
    auto add = [&budget](std::string_view name, std::size_t value_bytes) {
        if (!budget.add_field(name.size(), value_bytes)) {
            throw std::invalid_argument("WebSocket client fields too large");
        }
    };
    add(version_header_name, 2);
    if (plan.subprotocol_bytes_ != 0) {
        add(protocol_header_name, plan.subprotocol_bytes_);
    }
    if (offer.enabled_) {
        plan.static_extension_ = static_deflate_extension_storage(config.deflate_);
        if (plan.static_extension_ != nullptr) {
            add(extensions_header_name, plan.static_extension_->size() - extensions_header_name.size());
        } else {
            const auto& encoded = plan.encoded_extension_.emplace(offer);
            plan.client_window_bits_suffix_ =
                config.deflate_.offer_client_max_window_bits_ && !offer.client_max_window_bits_;
            add(extensions_header_name,
                encoded.view().size() + (plan.client_window_bits_suffix_ ? client_window_bits_suffix.size() : 0));
        }
    }
    return plan;
}

[[nodiscard]] http_header make_subprotocol_field(
    std::span<const std::string_view> subprotocols, std::size_t joined_bytes) {
    if (subprotocols.size() == 1U) {
        return http_header(protocol_header_name, subprotocols.front());
    }
    static constexpr std::size_t inline_joined_capacity = 256;
    if (joined_bytes <= inline_joined_capacity) {
        std::array<char, inline_joined_capacity> joined_buffer;
        std::size_t offset = 0;
        for (const auto protocol_view : subprotocols) {
            if (offset != 0) {
                joined_buffer[offset++] = ',';
                joined_buffer[offset++] = ' ';
            }
            std::memcpy(joined_buffer.data() + offset, protocol_view.data(), protocol_view.size());
            offset += protocol_view.size();
        }
        return http_header(protocol_header_name, std::string_view(joined_buffer.data(), offset));
    }
    std::string joined;
    joined.reserve(joined_bytes);
    for (const auto protocol_view : subprotocols) {
        if (!joined.empty()) {
            joined.append(", ");
        }
        joined.append(protocol_view);
    }
    return http_header(protocol_header_name, joined);
}

// The static offer (a private, storage-borrowing http_header) is built by the
// constructor; this handles the encoded offers that need owned storage.
[[nodiscard]] http_header make_extension_field(const client_offer_plan& plan) {
    const auto encoded = plan.encoded_extension_->view();
    if (!plan.client_window_bits_suffix_) {
        return http_header(extensions_header_name, encoded);
    }
    std::array<char, 128> offer_buffer;
    std::memcpy(offer_buffer.data(), encoded.data(), encoded.size());
    std::memcpy(offer_buffer.data() + encoded.size(), client_window_bits_suffix.data(), client_window_bits_suffix.size());
    return http_header(extensions_header_name,
        std::string_view(offer_buffer.data(), encoded.size() + client_window_bits_suffix.size()));
}
}  // namespace
void websocket_client_negotiation::validate_configuration(websocket_client_negotiation_config_view config) {
    (void)make_client_offer_plan(config);
}
websocket_client_negotiation::websocket_client_negotiation(websocket_client_negotiation_config_view config)
    : deflate_(config.deflate_) {
    const auto plan = make_client_offer_plan(config);
    fields_[field_count_++] = http_header(version_storage.data(), version_header_name.size(), 2);
    if (plan.subprotocol_bytes_ != 0) {
        fields_[field_count_] = make_subprotocol_field(config.subprotocols_.view(), plan.subprotocol_bytes_);
        offered_subprotocols_ = fields_[field_count_++].value();
    }
    if (config.deflate_.compression_.enabled_) {
        fields_[field_count_++] = plan.static_extension_ != nullptr
                                      ? http_header(plan.static_extension_->data(), extensions_header_name.size(),
                                            plan.static_extension_->size() - extensions_header_name.size())
                                      : make_extension_field(plan);
    }
}
std::variant<websocket_client_negotiation_result_view, error_type>
websocket_client_negotiation::validate_fields(std::span<const http_header> fields_value) const {
    return detail::validate_websocket_client_selection(*this, fields_value, false);
}

std::variant<websocket_client_negotiation_result_view, error_type> validate_extended_connect_websocket_client_response(
    const websocket_client_negotiation& negotiation, const http_client_response_head& head, bool stream_open) {
    if (!head.status().is_successful()) {
        return error_type::response_status;
    }
    if (!stream_open) {
        return error_type::stream_closed;
    }
    return detail::validate_websocket_client_selection(negotiation, head.headers(), true);
}

namespace detail {
std::variant<websocket_client_negotiation_result_view, error_type> validate_websocket_client_selection(
    const websocket_client_negotiation& negotiation, std::span<const http_header> fields, bool extended) {
    return validate(fields, negotiation.offered_subprotocols_, negotiation.deflate_, extended);
}
}  // namespace detail
}  // namespace ruvia
