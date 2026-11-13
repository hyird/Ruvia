#include "ruvia/http/http_request.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_client_redirect.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_target_origin.h"
#include "ruvia/http/url_encoding.h"

#include "field/http_connection_fields.h"
#include "http_wire_sink.h"
#include "parser/http_uri_grammar.h"

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
