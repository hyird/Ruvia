#include "ruvia/http/http_request.h"

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zlib.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>
#undef ZSTD_STATIC_LINKING_ONLY

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"

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

// The Content-Encoding field itself (RFC 9110 sections 8.4 and 5.6.1): the token
// each coding is spelled with, the list-grammar accumulator across field lines,
// and which codec a decode or encode of that coding runs on.

namespace ruvia {

std::string_view http_content_coding_token(http_content_coding coding) noexcept {
    switch (coding) {
        case http_content_coding::brotli:
            return "br";
        case http_content_coding::zstd:
            return "zstd";
        case http_content_coding::gzip:
            return "gzip";
        case http_content_coding::deflate:
            return "deflate";
        case http_content_coding::identity:
            return "identity";
    }
    return {};
}

std::optional<http_content_coding> http_content_coding_from_token(std::string_view token) noexcept {
    switch (token.size()) {
        case 2:
            if (http_ascii_equals_ignore_case(token, "br")) {
                return http_content_coding::brotli;
            }
            break;
        case 4:
            if (http_ascii_equals_ignore_case(token, "gzip")) {
                return http_content_coding::gzip;
            }
            if (http_ascii_equals_ignore_case(token, "zstd")) {
                return http_content_coding::zstd;
            }
            break;
        case 6:
            if (http_ascii_equals_ignore_case(token, "x-gzip")) {
                return http_content_coding::gzip;
            }
            break;
        case 7:
            if (http_ascii_equals_ignore_case(token, "deflate")) {
                return http_content_coding::deflate;
            }
            break;
        case 8:
            if (http_ascii_equals_ignore_case(token, "identity")) {
                return http_content_coding::identity;
            }
            break;
        default:
            break;
    }
    return std::nullopt;
}

}  // namespace ruvia

namespace ruvia::detail {

void http_content_coding_field_parser::update(std::string_view value) noexcept {
    if (invalid_) {
        return;
    }
    // Content-Encoding members are tokens (RFC 9110 section 8.4.1).
    invalid_ = visit_http_content_coding_field(value, http_field_list_role::recipient,
                   [this](std::string_view, std::optional<http_content_coding> coding) noexcept {
                       if (coding.has_value() && *coding == http_content_coding::identity) {
                           return true;
                       }
                       if (codings_.size_ == max_content_codings) {
                           // Keep scanning: later malformed members still invalidate the field.
                           depth_exceeded_ = true;
                       } else if (coding.has_value()) {
                           codings_.values_[codings_.size_++] = *coding;
                       } else {
                           ++codings_.size_;
                       }
                       if (!coding.has_value()) {
                           unknown_coding_ = true;
                       }
                       return true;
                   }) != http_field_list_parse_status::ok;
}

http_content_coding_field_result http_content_coding_field_parser::finish() const noexcept {
    if (invalid_) {
        return http_content_coding_field_result_access::invalid();
    }
    if (depth_exceeded_) {
        return http_content_coding_field_result_access::unsupported(true);
    }
    if (unknown_coding_) {
        return http_content_coding_field_result_access::unsupported(false);
    }
    return http_content_coding_field_result_access::coding(codings_);
}

bool is_valid_http_content_encoding_field_value(std::string_view value, http_field_list_role role) noexcept {
    return http_visit_token_list(value, role, [](std::string_view) noexcept { return true; }) ==
           http_field_list_parse_status::ok;
}

}  // namespace ruvia::detail

namespace ruvia {

http_content_coding_field_result parse_http_content_coding(
    std::string_view value) noexcept {
    detail::http_content_coding_field_parser parser;
    parser.update(value);
    return parser.finish();
}

http_content_coding_field_result parse_http_content_coding_headers(
    std::span<const http_header> headers) noexcept {
    return detail::http_content_coding_from_headers(headers);
}

http_content_coding_field_result parse_http_content_coding_headers(
    const http_response_headers& headers) noexcept {
    detail::http_content_coding_field_parser parser;
    const auto* const first = detail::http_response_header_state_access::known_header(
        headers, detail::response_header_content_encoding);
    if (first == nullptr) {
        return parser.finish();
    }
    if (!detail::http_response_header_access::append(*first)) {
        parser.update(first->value());
        return parser.finish();
    }
    for (const auto& header : headers) {
        if (detail::http_response_header_access::known_bit(header) ==
            detail::response_header_content_encoding) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

http_content_coding_field_result request_content_coding(const http_request& request) noexcept {
    detail::http_content_coding_field_parser parser;
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::content_encoding, [&parser](std::string_view value) noexcept {
            parser.update(value);
            return true;
        });
    return parser.finish();
}

namespace {

[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_content_stage(
    http_content_coding coding, std::string_view input, http_content_decode_options options) {
    // A zero-length coded representation carries no content: user agents treat
    // it as empty rather than as a truncated coding stream. An out-of-range coding
    // has no token and stays unsupported even for empty input.
    if (input.empty() && !http_content_coding_token(coding).empty()) {
        return std::string{};
    }
    switch (coding) {
        case http_content_coding::gzip:
            return detail::decode_gzip_content(input, options.max_decoded_bytes_);
        case http_content_coding::deflate:
            return detail::decode_deflate_content(input, options.max_decoded_bytes_);
        case http_content_coding::brotli:
            return detail::decode_brotli_content(input, options.max_decoded_bytes_);
        case http_content_coding::zstd:
            return detail::decode_zstd_content(input, options.max_decoded_bytes_);
        case http_content_coding::identity:
            // Identity stages are skipped by decode_http_content.
            break;
    }
    return http_content_decode_error::unsupported_coding;
}

}  // namespace

std::optional<std::string> encode_http_content(
    http_content_coding coding, std::string_view input, http_content_encode_options options) {
    switch (coding) {
        case http_content_coding::brotli:
            return detail::encode_brotli_content(input, options.max_encoded_bytes_);
        case http_content_coding::zstd:
            return detail::encode_zstd_content(input, options.max_encoded_bytes_);
        case http_content_coding::gzip:
            return detail::encode_gzip_content(input, options.max_encoded_bytes_);
        case http_content_coding::deflate:
            return detail::encode_deflate_content(input, options.max_encoded_bytes_);
        case http_content_coding::identity:
            if (input.size() > options.max_encoded_bytes_) {
                return std::nullopt;
            }
            return std::string(input);
    }
    throw http_content_codec_error("unsupported content coding");
}

std::optional<std::string> encode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_encode_options options) {
    // Nothing is allocated until a stage produces content.
    std::optional<std::string> current;
    for (const auto coding : codings) {
        // identity transforms nothing; only the size limit below applies to it.
        if (coding == http_content_coding::identity) {
            continue;
        }
        const std::string_view source_value = current.has_value() ? std::string_view(*current) : input;
        auto stage = encode_http_content(coding, source_value, options);
        if (!stage.has_value()) {
            return std::nullopt;
        }
        current = std::move(stage);
    }
    if (current.has_value()) {
        return current;
    }
    return encode_http_content(http_content_coding::identity, input, options);
}

std::variant<std::string, http_content_decode_error> decode_http_content(
    http_content_coding coding, std::string_view input, http_content_decode_options options) {
    return decode_http_content(std::span<const http_content_coding>(&coding, 1), input, options);
}

std::variant<std::string, http_content_decode_error> decode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_decode_options options) {
    // Nothing is allocated until a stage produces content, so an oversized
    // identity body is rejected before any allocation.
    std::optional<std::string> current;
    for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
        // identity transforms nothing; only the size limit below applies to it.
        if (*coding == http_content_coding::identity) {
            continue;
        }
        const std::string_view source_value = current.has_value() ? std::string_view(*current) : input;
        auto stage = decode_content_stage(*coding, source_value, options);
        auto* content = std::get_if<std::string>(&stage);
        if (content == nullptr) {
            return std::get<http_content_decode_error>(stage);
        }
        current = std::move(*content);
    }
    if (current.has_value()) {
        return std::move(*current);
    }
    if (input.size() > options.max_decoded_bytes_) {
        return http_content_decode_error::decoded_size_exceeded;
    }
    return std::string(input);
}

namespace {

[[nodiscard]] bool is_compress_coding_token(std::string_view token) noexcept {
    return http_ascii_equals_ignore_case(token, "compress") ||
           http_ascii_equals_ignore_case(token, "x-compress");
}

[[nodiscard]] int& explicit_quality(http_response_coding_qualities& qualities, http_content_coding coding) noexcept {
    switch (coding) {
        case http_content_coding::gzip:
            return qualities.gzip_.explicit_quality_;
        case http_content_coding::deflate:
            return qualities.deflate_.explicit_quality_;
        case http_content_coding::brotli:
            return qualities.brotli_.explicit_quality_;
        case http_content_coding::zstd:
            return qualities.zstd_.explicit_quality_;
        case http_content_coding::identity:
            break;
    }
    return qualities.identity_.explicit_quality_;
}

// Hands every nonempty Accept-Encoding member's coding token and weight to
// `visitor`. A repeated coding keeps its highest weight.
template <typename visitor_type>
void visit_accept_encoding_members(std::string_view accept_encoding, visitor_type&& visitor) noexcept {
    detail::http_visit_comma_separated_quoted(accept_encoding, [&visitor](std::string_view item) noexcept {
        visitor(detail::http_header_token_before_parameters(item), detail::http_weight_parameter(item));
        return true;
    });
}

}  // namespace

void http_accepted_encoding_quality::update(
    std::string_view accept_encoding, std::string_view coding) noexcept {
    const bool gzip_coding = http_is_gzip_coding_token(coding);
    const bool compress_coding = is_compress_coding_token(coding);
    visit_accept_encoding_members(accept_encoding,
        [coding, gzip_coding, compress_coding, this](std::string_view token, int weight) noexcept {
            const bool explicit_match = gzip_coding       ? http_is_gzip_coding_token(token)
                                        : compress_coding ? is_compress_coding_token(token)
                                                          : http_ascii_equals_ignore_case(token, coding);
            if (explicit_match) {
                explicit_quality_ = std::max(explicit_quality_, weight);
            } else if (token == "*") {
                wildcard_quality_ = std::max(wildcard_quality_, weight);
            }
        });
}

bool http_accepts_encoding(std::string_view accept_encoding, std::string_view coding) noexcept {
    if (accept_encoding.empty()) {
        return http_ascii_equals_ignore_case(coding, "identity");
    }
    http_accepted_encoding_quality quality;
    quality.update(accept_encoding, coding);
    return quality.accepts(http_ascii_equals_ignore_case(coding, "identity"));
}

void http_response_coding_qualities::update(std::string_view accept_encoding) noexcept {
    field_present_ = true;
    visit_accept_encoding_members(accept_encoding, [this](std::string_view token, int weight) noexcept {
        has_non_empty_item_ = true;
        if (const auto coding = http_content_coding_from_token(token)) {
            auto& quality = explicit_quality(*this, *coding);
            quality = std::max(quality, weight);
        } else if (token == "*") {
            for (auto* coding_quality : {&gzip_, &deflate_, &brotli_, &zstd_, &identity_}) {
                coding_quality->wildcard_quality_ = std::max(coding_quality->wildcard_quality_, weight);
            }
        }
    });
}

bool http_accepts_encoding(const http_request& request, std::string_view coding) noexcept {
    http_accepted_encoding_quality quality;
    bool present = false;
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::accept_encoding, [&](std::string_view value) noexcept {
            present = true;
            quality.update(value, coding);
            return true;
        });
    return !present || quality.accepts(http_ascii_equals_ignore_case(coding, "identity"));
}

bool http_response_coding_selection::accepts(std::string_view coding, const http_request& request) const noexcept {
    if (const auto known = http_content_coding_from_token(coding)) {
        return accepts(*known);
    }
    return http_accepts_encoding(request, coding);
}

http_response_content_coding_status evaluate_http_response_content_coding(
    const http_response_coding_selection& selection, const http_response& response,
    const http_request* request) {
    if (!detail::response_has_known_header(response, detail::response_header_content_encoding)) {
        return http_response_content_coding_status::absent;
    }
    const bool evaluate_extensions = request != nullptr &&
                                     detail::http_request_access::has_known_header(*request, detail::http_field_kind::accept_encoding);
    struct extension_quality final {
        std::string_view token_;
        http_accepted_encoding_quality quality_;
    };
    std::array<extension_quality, 8> local{};
    std::size_t local_size = 0;
    std::vector<extension_quality> overflow;
    const auto extensions = [&]() noexcept -> std::span<extension_quality> {
        return overflow.empty() ? std::span(local.data(), local_size) : std::span(overflow);
    };
    bool accepted = true;
    bool invalid = false;
    const auto remember_extension = [&](std::string_view token) {
        for (const auto& extension : extensions()) {
            if (http_ascii_equals_ignore_case(extension.token_, token) ||
                (is_compress_coding_token(extension.token_) && is_compress_coding_token(token))) {
                return;
            }
        }
        if (overflow.empty() && local_size < local.size()) {
            local[local_size++] = {token, {}};
        } else {
            if (overflow.empty()) {
                overflow.reserve(local.size() * 2);
                overflow.insert(overflow.end(), local.begin(), local.end());
            }
            overflow.push_back({token, {}});
        }
    };
    for (const auto& header : response.headers()) {
        if (detail::http_response_header_access::known_bit(header) != detail::response_header_content_encoding) {
            continue;
        }
        const auto status = detail::visit_http_content_coding_field(header.value(), detail::http_field_list_role::recipient,
            [&](std::string_view token, std::optional<http_content_coding> coding) {
                if (coding.has_value()) {
                    accepted = accepted && selection.accepts(*coding);
                } else if (evaluate_extensions) {
                    remember_extension(token);
                }
                return true;
            });
        invalid = invalid || status != detail::http_field_list_parse_status::ok;
    }
    if (invalid) {
        return http_response_content_coding_status::invalid;
    }
    if (!accepted) {
        return http_response_content_coding_status::not_acceptable;
    }
    if (!extensions().empty()) {
        bool field_present = false;
        (void)detail::http_request_access::visit_known_headers(
            *request, detail::http_field_kind::accept_encoding, [&](std::string_view value) noexcept {
                field_present = true;
                visit_accept_encoding_members(value, [&](std::string_view token, int weight) noexcept {
                    for (auto& extension : extensions()) {
                        const bool matches = http_ascii_equals_ignore_case(extension.token_, token) ||
                                             (is_compress_coding_token(extension.token_) && is_compress_coding_token(token));
                        if (matches) {
                            extension.quality_.explicit_quality_ = std::max(extension.quality_.explicit_quality_, weight);
                        } else if (token == "*") {
                            extension.quality_.wildcard_quality_ = std::max(extension.quality_.wildcard_quality_, weight);
                        }
                    }
                });
                return true;
            });
        if (field_present) {
            for (const auto& extension : extensions()) {
                if (!extension.quality_.accepts()) {
                    return http_response_content_coding_status::not_acceptable;
                }
            }
        }
    }
    return http_response_content_coding_status::accepted;
}

http_response_coding_selection select_http_accept_encoding(
    std::string_view accept_encoding, std::span<const http_content_coding> candidates) noexcept {
    http_response_coding_qualities qualities;
    qualities.update(accept_encoding);
    return http_response_coding_selection::select(qualities, candidates);
}

http_response_coding_selection select_http_accept_encoding(
    std::span<const http_header> request_headers,
    std::span<const http_content_coding> candidates) noexcept {
    http_response_coding_qualities qualities;
    for (const auto& header : request_headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Accept-Encoding")) {
            qualities.update(header.value());
        }
    }
    return http_response_coding_selection::select(qualities, candidates);
}

http_response_coding_selection select_http_accept_encoding(
    std::span<const http_header_view> request_headers,
    std::span<const http_content_coding> candidates) noexcept {
    http_response_coding_qualities qualities;
    for (const auto& header : request_headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Accept-Encoding")) {
            qualities.update(header.value());
        }
    }
    return http_response_coding_selection::select(qualities, candidates);
}

http_response_coding_selection select_http_accept_encoding(
    const http_request& request, std::span<const http_content_coding> candidates) noexcept {
    return http_response_coding_selection_for(request).select(candidates);
}

std::optional<http_content_coding> negotiate_http_content_coding(
    std::string_view accept_encoding, std::span<const http_content_coding> candidates) noexcept {
    const auto selection = select_http_accept_encoding(accept_encoding, candidates);
    return selection.acceptable() ? std::optional(selection.coding()) : std::nullopt;
}

std::optional<http_content_coding> negotiate_http_content_coding(
    std::span<const http_header> request_headers,
    std::span<const http_content_coding> candidates) noexcept {
    const auto selection = select_http_accept_encoding(request_headers, candidates);
    return selection.acceptable() ? std::optional(selection.coding()) : std::nullopt;
}

std::optional<http_content_coding> negotiate_http_content_coding(
    std::span<const http_header_view> request_headers,
    std::span<const http_content_coding> candidates) noexcept {
    const auto selection = select_http_accept_encoding(request_headers, candidates);
    return selection.acceptable() ? std::optional(selection.coding()) : std::nullopt;
}

std::optional<http_content_coding> negotiate_http_content_coding(
    const http_request& request, std::span<const http_content_coding> candidates) noexcept {
    const auto selection = select_http_accept_encoding(request, candidates);
    return selection.acceptable() ? std::optional(selection.coding()) : std::nullopt;
}

bool is_http_media_type_compressible(std::string_view content_type_value) noexcept {
    const auto media_type = http_media_type_only(content_type_value);
    if (media_type.empty()) {
        return false;
    }
    const auto starts_with = [&](std::string_view prefix) noexcept {
        return media_type.size() >= prefix.size() &&
               http_ascii_equals_ignore_case(media_type.substr(0, prefix.size()), prefix);
    };
    const auto ends_with = [&](std::string_view suffix) noexcept {
        return media_type.size() >= suffix.size() &&
               http_ascii_equals_ignore_case(
                   media_type.substr(media_type.size() - suffix.size()), suffix);
    };
    if (ends_with("zip")) {
        return false;
    }
    if (starts_with("text/") || ends_with("+json") || ends_with("+xml")) {
        return true;
    }
    static constexpr std::array<std::string_view, 14> compressible_types{
        "application/json",
        "application/javascript",
        "application/x-javascript",
        "application/ecmascript",
        "application/wasm",
        "application/xml",
        "application/x-ndjson",
        "application/vnd.ms-fontobject",
        "font/ttf",
        "font/otf",
        "application/x-font-ttf",
        "application/x-font-otf",
        "image/x-icon",
        "image/vnd.microsoft.icon",
    };
    for (const auto type : compressible_types) {
        if (http_ascii_equals_ignore_case(media_type, type)) {
            return true;
        }
    }
    return false;
}
}  // namespace ruvia

namespace ruvia::detail {

namespace {

// zlib requests `items * size` bytes. Zero-sized and overflowing requests are
// refused with nullptr, which is how zlib expects allocation failure.
voidpf zlib_allocate(voidpf opaque, uInt items, uInt size) noexcept {
    if (items == 0 || size == 0) {
        return nullptr;
    }
    const auto item_bytes = static_cast<std::size_t>(items);
    const auto size_bytes = static_cast<std::size_t>(size);
    if (item_bytes > (std::numeric_limits<std::size_t>::max)() / size_bytes) {
        return nullptr;
    }
    return http_c_library_memory::allocate(item_bytes * size_bytes, opaque);
}

[[nodiscard]] z_stream make_zlib_stream(http_c_library_memory& allocation) noexcept {
    z_stream stream{};
    stream.zalloc = &zlib_allocate;
    stream.zfree = &http_c_library_memory::deallocate_opaque_first;
    stream.opaque = &allocation;
    return stream;
}

}  // namespace

zlib_inflate_stream::zlib_inflate_stream(int window_bits, std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      stream_(make_zlib_stream(allocation_)) {
    if (inflateInit2(&stream_, window_bits) != Z_OK) {
        allocation_.rethrow_if_failed();
    }
}

zlib_inflate_stream::~zlib_inflate_stream() {
    if (initialized()) {
        (void)inflateEnd(&stream_);
    }
}

bool zlib_inflate_stream::reset() noexcept {
    allocation_.clear_allocation_failure();
    return initialized() && inflateReset(&stream_) == Z_OK;
}

zlib_deflate_stream::zlib_deflate_stream(
    int level, int window_bits, int mem_level, int strategy, std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      stream_(make_zlib_stream(allocation_)) {
    if (deflateInit2(&stream_, level, Z_DEFLATED, window_bits, mem_level, strategy) != Z_OK) {
        allocation_.rethrow_if_failed();
    }
}

zlib_deflate_stream::zlib_deflate_stream(zlib_content_format format, std::size_t max_memory_bytes)
    : zlib_deflate_stream(Z_DEFAULT_COMPRESSION,
          format == zlib_content_format::gzip ? zlib_gzip_window_bits : zlib_deflate_window_bits,
          zlib_default_mem_level, Z_DEFAULT_STRATEGY, max_memory_bytes) {}

zlib_deflate_stream::~zlib_deflate_stream() {
    if (initialized()) {
        (void)deflateEnd(&stream_);
    }
}

bool zlib_deflate_stream::reset() noexcept {
    allocation_.clear_allocation_failure();
    return initialized() && deflateReset(&stream_) == Z_OK;
}

bool zlib_deflate_stream::encode(std::string_view input, std::string& output, int flush,
    std::size_t max_encoded_bytes) {
    if (output.size() > max_encoded_bytes) {
        return false;
    }
    const zlib_borrow_scope borrow(stream_);
    std::size_t supplied = 0;
    // Uninitialized scratch: only the bytes zlib produced are copied out.
    std::array<char, 16384> buffer;
    for (;;) {
        zlib_refill_input(stream_, input, supplied);
        // The requested flush applies once the last input window is handed
        // over: after Z_FINISH zlib accepts no further input.
        const bool last_window = supplied == input.size();
        const int operation = last_window ? flush : Z_NO_FLUSH;
        // At the cap, a one-byte probe distinguishes "already complete" from
        // "more output pending" without letting a byte past the cap through.
        const auto room = max_encoded_bytes - output.size();
        const bool probing = room == 0;
        const auto writable = probing ? std::size_t{1} : (std::min)(buffer.size(), room);
        const auto before_input = stream_.avail_in;
        stream_.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream_.avail_out = static_cast<uInt>(writable);
        const int status = deflate(&stream_, operation);
        const bool input_consumed = last_window && stream_.avail_in == 0;
        // Repeating a completed flush has no work left; zlib reports
        // Z_BUF_ERROR without invalidating the stream.
        if (status == Z_BUF_ERROR && operation != Z_FINISH && input_consumed) {
            return true;
        }
        if (status != Z_OK && status != Z_STREAM_END) {
            allocation_.rethrow_if_failed();
            throw http_content_codec_error("zlib content encoding failed");
        }
        const auto produced = writable - stream_.avail_out;
        if (probing && produced != 0) {
            return false;
        }
        output.append(buffer.data(), produced);
        if (status == Z_STREAM_END ||
            (operation != Z_FINISH && input_consumed && stream_.avail_out != 0)) {
            return true;
        }
        if (probing) {
            return false;
        }
        if (produced == 0 && stream_.avail_in == before_input) {
            throw http_content_codec_error("zlib content encoder made no progress");
        }
    }
}

}  // namespace ruvia::detail

// gzip (RFC 1952) and zlib-wrapped deflate through zlib, with tracked working memory.

namespace ruvia::detail {

static std::variant<std::string, http_content_decode_error> decode_zlib_content(std::string_view input,
    std::size_t max_decoded_bytes, int window_bits,
    bool allow_concatenated_members) {
    std::string output;
    zlib_inflate_stream inflater(window_bits);
    if (!inflater.initialized()) {
        throw http_content_codec_error("zlib decoder initialization failed");
    }
    auto& stream = inflater.native();

    std::size_t supplied = 0;
    char buffer[16384];
    for (;;) {
        zlib_refill_input(stream, input, supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = static_cast<uInt>(sizeof(buffer));
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
            inflater.rethrow_allocation_failure();
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (!append_decoded_bytes(output, buffer, produced, max_decoded_bytes)) {
            return http_content_decode_error::decoded_size_exceeded;
        }

        if (status == Z_STREAM_END) {
            // RFC 1952 gzip data is a series of members. Preserve any input
            // already supplied to zlib, reset only the member state, and keep
            // decoding until the exact HTTP content boundary is consumed.
            zlib_refill_input(stream, input, supplied);
            if (stream.avail_in == 0 && supplied == input.size()) {
                return output;
            }
            if (!allow_concatenated_members) {
                return http_content_decode_error::invalid_content;
            }
            auto* next_input = stream.next_in;
            const auto available_input = stream.avail_in;
            const int reset = inflateReset2(&stream, window_bits);
            if (reset != Z_OK) {
                throw http_content_codec_error("failed to reset zlib content decoder");
            }
            stream.next_in = next_input;
            stream.avail_in = available_input;
            continue;
        }
        if (status == Z_MEM_ERROR) {
            throw http_content_codec_error("zlib decoder ran out of memory");
        }
        if (status != Z_OK && status != Z_BUF_ERROR) {
            return http_content_decode_error::invalid_content;
        }

        const bool progressed = produced != 0 || stream.avail_in != before_input;
        if (!progressed) {
            if (stream.avail_in == 0 && supplied < input.size()) {
                continue;
            }
            return http_content_decode_error::invalid_content;
        }
    }
}

std::optional<std::string> encode_zlib_content(zlib_deflate_stream& deflater,
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content_with_stream(deflater, Z_FINISH, input, max_encoded_bytes);
}
std::variant<std::string, http_content_decode_error> decode_gzip_content(
    std::string_view input, std::size_t max_decoded_bytes) {
    return decode_zlib_content(input, max_decoded_bytes, zlib_gzip_window_bits, true);
}

std::variant<std::string, http_content_decode_error> decode_deflate_content(
    std::string_view input, std::size_t max_decoded_bytes) {
    return decode_zlib_content(input, max_decoded_bytes, zlib_deflate_window_bits, false);
}

std::optional<std::string> encode_gzip_content(
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content<zlib_deflate_stream>(
        "zlib encoder initialization failed", Z_FINISH, input, max_encoded_bytes, zlib_content_format::gzip);
}

std::optional<std::string> encode_deflate_content(
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content<zlib_deflate_stream>("zlib encoder initialization failed", Z_FINISH, input,
        max_encoded_bytes, zlib_content_format::deflate);
}

}  // namespace ruvia::detail

// Brotli (RFC 7932) state with tracked working memory.
// Brotli retains the allocator context as its opaque pointer, so these types are
// pinned: owners embed them in storage whose address never changes.
//
// Construction rethrows an allocation failure captured during initialization.
// Any other Brotli refusal leaves the stream uninitialized so each owner maps it
// into its own error contract.

namespace ruvia::detail {

// Decoder whose native working memory is bounded by the caller's decoded-size
// cap rather than by the window and metablock sizes the peer declares: a
// declared ring buffer that the cap could never fill is refused, and
// native_budget_exceeded() then reports a cap violation instead of a codec
// failure.
class brotli_decoder_stream final {
public:
    explicit brotli_decoder_stream(
        std::size_t max_decoded_bytes,
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    ~brotli_decoder_stream();
    brotli_decoder_stream(const brotli_decoder_stream&) = delete;
    brotli_decoder_stream& operator=(const brotli_decoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return state_ != nullptr;
    }
    [[nodiscard]] BrotliDecoderState* native() const noexcept {
        return state_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_if_failed();
    }
    [[nodiscard]] bool native_budget_exceeded() const noexcept {
        return allocation_.budget_exceeded();
    }

private:
    http_c_library_memory allocation_;
    BrotliDecoderState* state_;
};

// LZ77 window exponents. Whole-buffer encoding is bounded by its input and
// released at once, so it keeps the library default; a stream encoder lives as
// long as its response (SSE), so it uses Pingora's 512 KiB window.
inline constexpr int brotli_whole_buffer_window_bits = BROTLI_DEFAULT_WINDOW;
inline constexpr int brotli_stream_window_bits = 19;

// Encoder at quality 5, shared by whole-buffer and streaming HTTP content coding.
class brotli_encoder_stream final {
public:
    explicit brotli_encoder_stream(
        int window_bits,
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    ~brotli_encoder_stream();
    brotli_encoder_stream(const brotli_encoder_stream&) = delete;
    brotli_encoder_stream& operator=(const brotli_encoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return state_ != nullptr;
    }
    // Consumes the input and drains the requested operation into output. False
    // means the total output budget was exceeded before appending a block.
    // After false or an exception, the owner must discard/rebuild this stream.
    [[nodiscard]] bool encode(std::string_view input, std::string& output,
        BrotliEncoderOperation operation, std::size_t max_encoded_bytes);

private:
    http_c_library_memory allocation_;
    BrotliEncoderState* state_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

namespace {

inline constexpr std::uint32_t http_brotli_quality = 5;

// Upper bound of the per-metablock Huffman groups (256 trees each of the
// literal, command and 520-symbol distance alphabets at alphabet + 376
// four-byte entries, about 2.7 MB in the reference decoder) plus context maps,
// block-type trees and the state itself. It does not depend on the window.
inline constexpr std::size_t brotli_decoder_table_allowance = 3U * 1024U * 1024U;

// The reference decoder sizes its ring buffer as the smallest power of two
// covering the bytes already decoded plus the declared metablock length, and
// grows it by doubling, holding the old ring during the copy. Content that
// fits `max_decoded_bytes` therefore never needs more than three times that
// cap (1 KiB floor) plus write-ahead slack; a larger ring can only serve
// content the cap rejects anyway.
[[nodiscard]] std::size_t brotli_decoder_native_budget(std::size_t max_decoded_bytes) noexcept {
    constexpr auto unlimited = (std::numeric_limits<std::size_t>::max)();
    constexpr std::size_t ring_floor = 1024;
    constexpr std::size_t ring_slack = 2 * 1024;
    const auto ring_bytes = (std::max)(max_decoded_bytes, ring_floor);
    if (ring_bytes > (unlimited - ring_slack - brotli_decoder_table_allowance) / 3) {
        return unlimited;
    }
    return 3 * ring_bytes + ring_slack + brotli_decoder_table_allowance;
}

[[nodiscard]] bool brotli_allocation_failure(BrotliDecoderErrorCode error) noexcept {
    switch (error) {
        case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES:
        case BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS:
        case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP:
        case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1:
        case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2:
        case BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES:
            return true;
        default:
            return false;
    }
}
}  // namespace

brotli_decoder_stream::brotli_decoder_stream(
    std::size_t max_decoded_bytes, std::size_t max_memory_bytes)
    : allocation_(
          (std::min)(brotli_decoder_native_budget(max_decoded_bytes), max_memory_bytes),
          max_memory_bytes < brotli_decoder_native_budget(max_decoded_bytes)),
      state_(BrotliDecoderCreateInstance(
          &http_c_library_memory::allocate_opaque_first, &http_c_library_memory::deallocate_opaque_first, &allocation_)) {
    if (state_ == nullptr) {
        allocation_.rethrow_if_failed();
    }
}

brotli_decoder_stream::~brotli_decoder_stream() {
    if (state_ != nullptr) {
        BrotliDecoderDestroyInstance(state_);
    }
}

brotli_encoder_stream::brotli_encoder_stream(int window_bits, std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      state_(BrotliEncoderCreateInstance(
          &http_c_library_memory::allocate_opaque_first, &http_c_library_memory::deallocate_opaque_first, &allocation_)) {
    if (state_ != nullptr &&
        (BrotliEncoderSetParameter(state_, BROTLI_PARAM_QUALITY, http_brotli_quality) != BROTLI_TRUE ||
            BrotliEncoderSetParameter(state_, BROTLI_PARAM_LGWIN, static_cast<std::uint32_t>(window_bits)) != BROTLI_TRUE)) {
        BrotliEncoderDestroyInstance(state_);
        state_ = nullptr;
    }
    if (state_ == nullptr) {
        allocation_.rethrow_if_failed();
    }
}

brotli_encoder_stream::~brotli_encoder_stream() {
    if (state_ != nullptr) {
        BrotliEncoderDestroyInstance(state_);
    }
}

bool brotli_encoder_stream::encode(std::string_view input, std::string& output,
    BrotliEncoderOperation operation, std::size_t max_encoded_bytes) {
    std::size_t available_input = input.size();
    const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
    for (;;) {
        const auto before_input = available_input;
        std::size_t available_output = 0;
        const auto compressed = BrotliEncoderCompressStream(state_, operation,
            &available_input, &next_input, &available_output, nullptr, nullptr);
        allocation_.rethrow_if_failed();
        if (compressed != BROTLI_TRUE) {
            throw http_content_codec_error("brotli encoding failed");
        }
        std::size_t produced = 0;
        const auto* bytes_value = BrotliEncoderTakeOutput(state_, &produced);
        if (output.size() > max_encoded_bytes || produced > max_encoded_bytes - output.size()) {
            return false;
        }
        // Consume the borrowed block directly into the caller's output before
        // any subsequent encoder call can invalidate it.
        if (produced != 0) {
            output.append(reinterpret_cast<const char*>(bytes_value), produced);
        }
        if (operation == BROTLI_OPERATION_FINISH) {
            if (BrotliEncoderIsFinished(state_) == BROTLI_TRUE) {
                return true;
            }
        } else if (available_input == 0 && BrotliEncoderHasMoreOutput(state_) != BROTLI_TRUE) {
            return true;
        }
        if (produced == 0 && available_input == before_input) {
            throw http_content_codec_error("brotli encoder made no progress");
        }
    }
}

std::variant<std::string, http_content_decode_error> decode_brotli_content(
    std::string_view input, std::size_t max_decoded_bytes) {
    std::string output;
    brotli_decoder_stream decoder(max_decoded_bytes);
    if (!decoder.initialized()) {
        throw http_content_codec_error("brotli decoder initialization failed");
    }
    auto* state_value = decoder.native();

    const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t available_input = input.size();
    for (;;) {
        const auto before_input = available_input;
        std::size_t available_output = 0;
        const auto result_value = BrotliDecoderDecompressStream(
            state_value, &available_input, &next_input, &available_output, nullptr, nullptr);
        bool produced_output = false;
        // The ring buffer can expose more than one contiguous block. Consume
        // each borrow before the next decoder call invalidates it.
        while (BrotliDecoderHasMoreOutput(state_value) == BROTLI_TRUE) {
            std::size_t produced = 0;
            const auto* bytes_value = BrotliDecoderTakeOutput(state_value, &produced);
            if (!append_decoded_bytes(
                    output, reinterpret_cast<const char*>(bytes_value), produced, max_decoded_bytes)) {
                return http_content_decode_error::decoded_size_exceeded;
            }
            produced_output = produced_output || produced != 0;
        }
        if (result_value == BROTLI_DECODER_RESULT_SUCCESS) {
            // RFC 7932 defines one Brotli stream. Its decoder deliberately does
            // not over-consume, so remaining bytes are not part of this content.
            if (available_input == 0) {
                return output;
            }
            return http_content_decode_error::invalid_content;
        }
        if (result_value == BROTLI_DECODER_RESULT_ERROR) {
            // A ring buffer refused by the native budget was declared larger
            // than the cap allows the content to be.
            if (decoder.native_budget_exceeded()) {
                return http_content_decode_error::decoded_size_exceeded;
            }
            decoder.rethrow_allocation_failure();
            if (brotli_allocation_failure(BrotliDecoderGetErrorCode(state_value))) {
                throw std::bad_alloc();
            }
            return http_content_decode_error::invalid_content;
        }
        const bool progressed = produced_output || available_input != before_input;
        if (!progressed ||
            (result_value == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && available_input == 0)) {
            return http_content_decode_error::invalid_content;
        }
    }
}

std::optional<std::string> encode_brotli_content(
    brotli_encoder_stream& encoder, std::string_view input,
    std::size_t max_encoded_bytes) {
    return encode_whole_content_with_stream(
        encoder, BROTLI_OPERATION_FINISH, input, max_encoded_bytes);
}

std::optional<std::string> encode_brotli_content(
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content<brotli_encoder_stream>("brotli encoder initialization failed",
        BROTLI_OPERATION_FINISH, input, max_encoded_bytes, brotli_whole_buffer_window_bits);
}

}  // namespace ruvia::detail

// zstd (RFC 8878) state with tracked working memory and
// whose window never exceeds the 8 MiB HTTP limit of RFC 9659. zstd
// retains the allocator context as its opaque pointer, so these types are
// pinned: owners embed them in storage whose address never changes.
//
// Construction rethrows an allocation failure captured during initialization.
// Any other zstd refusal leaves the stream uninitialized so each owner maps it
// into its own error contract.

namespace ruvia::detail {

class zstd_decoder_stream final {
public:
    explicit zstd_decoder_stream(
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    ~zstd_decoder_stream();
    zstd_decoder_stream(const zstd_decoder_stream&) = delete;
    zstd_decoder_stream& operator=(const zstd_decoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return stream_ != nullptr;
    }
    [[nodiscard]] ZSTD_DStream* native() const noexcept {
        return stream_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_if_failed();
    }

private:
    http_c_library_memory allocation_;
    ZSTD_DStream* stream_;
};

// Encoder at the default compression level, with the level's own window.
// reset() ends any frame in progress and keeps both the parameters and the
// working memory, so the next frame is identical to one from a new encoder.
// false means the owner must destroy it.
class zstd_encoder_stream final {
public:
    explicit zstd_encoder_stream(
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    ~zstd_encoder_stream();
    zstd_encoder_stream(const zstd_encoder_stream&) = delete;
    zstd_encoder_stream& operator=(const zstd_encoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return context_ != nullptr;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_if_failed();
    }
    [[nodiscard]] bool reset() noexcept;
    // The single zstd encode loop, shared by whole-buffer and streaming
    // coding: consumes the input and drains `operation` into output. False
    // means output would exceed `max_encoded_bytes` in total; no byte past
    // that cap is appended. After false or an exception, the owner must reset
    // or discard this stream.
    [[nodiscard]] bool encode(std::string_view input, std::string& output,
        ZSTD_EndDirective operation, std::size_t max_encoded_bytes);

private:
    http_c_library_memory allocation_;
    ZSTD_CCtx* context_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

namespace {

// RFC 9659 §3: HTTP zstd windows never exceed 8 MiB. Decoders must accept
// that window, so it is also the decoder's floor.
inline constexpr int http_zstd_window_log_max = 23;

[[noreturn]] void fail_zstd_encoding(const zstd_encoder_stream& encoder, std::size_t result_value) {
    encoder.rethrow_allocation_failure();
    throw http_content_codec_error(ZSTD_getErrorName(result_value));
}

}  // namespace

zstd_decoder_stream::zstd_decoder_stream(std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      stream_(ZSTD_createDStream_advanced(ZSTD_customMem{
          &http_c_library_memory::allocate_opaque_first, &http_c_library_memory::deallocate_opaque_first, &allocation_})) {
    if (stream_ != nullptr &&
        (ZSTD_isError(ZSTD_initDStream(stream_)) != 0 ||
            ZSTD_isError(ZSTD_DCtx_setParameter(stream_, ZSTD_d_windowLogMax, http_zstd_window_log_max)) != 0)) {
        (void)ZSTD_freeDStream(stream_);
        stream_ = nullptr;
    }
    if (stream_ == nullptr) {
        allocation_.rethrow_if_failed();
    }
}

zstd_decoder_stream::~zstd_decoder_stream() {
    if (stream_ != nullptr) {
        (void)ZSTD_freeDStream(stream_);
    }
}

zstd_encoder_stream::zstd_encoder_stream(std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      context_(ZSTD_createCCtx_advanced(ZSTD_customMem{
          &http_c_library_memory::allocate_opaque_first, &http_c_library_memory::deallocate_opaque_first, &allocation_})) {
    // The level picks its own window (and adapts it to a known source size).
    // RFC 9659's 8 MiB is only a ceiling; pinning it would also fix the
    // streaming input buffer at the window size for every pooled encoder.
    const bool window_exceeds_http_limit =
        ZSTD_getCParams(ZSTD_CLEVEL_DEFAULT, ZSTD_CONTENTSIZE_UNKNOWN, 0).windowLog >
        static_cast<unsigned>(http_zstd_window_log_max);
    if (context_ != nullptr &&
        (ZSTD_isError(ZSTD_CCtx_setParameter(context_, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT)) != 0 ||
            (window_exceeds_http_limit &&
                ZSTD_isError(ZSTD_CCtx_setParameter(context_, ZSTD_c_windowLog, http_zstd_window_log_max)) != 0))) {
        (void)ZSTD_freeCCtx(context_);
        context_ = nullptr;
    }
    if (context_ == nullptr) {
        allocation_.rethrow_if_failed();
    }
}

zstd_encoder_stream::~zstd_encoder_stream() {
    if (context_ != nullptr) {
        (void)ZSTD_freeCCtx(context_);
    }
}

bool zstd_encoder_stream::reset() noexcept {
    allocation_.clear_allocation_failure();
    return context_ != nullptr && ZSTD_isError(ZSTD_CCtx_reset(context_, ZSTD_reset_session_only)) == 0;
}

bool zstd_encoder_stream::encode(std::string_view input, std::string& output,
    ZSTD_EndDirective operation, std::size_t max_encoded_bytes) {
    if (output.size() > max_encoded_bytes) {
        return false;
    }
    ZSTD_inBuffer input_buffer{input.data(), input.size(), 0};
    // Uninitialized scratch: only the bytes zstd produced are copied out.
    std::array<char, 16384> buffer;
    for (;;) {
        // At the cap, a one-byte probe distinguishes "already complete" from
        // "more output pending" without letting a byte past the cap through.
        const auto room = max_encoded_bytes - output.size();
        const bool probing = room == 0;
        const auto writable = probing ? std::size_t{1} : (std::min)(buffer.size(), room);
        ZSTD_outBuffer output_buffer{buffer.data(), writable, 0};
        const auto before_input = input_buffer.pos;
        const auto remaining = ZSTD_compressStream2(context_, &output_buffer, &input_buffer, operation);
        if (ZSTD_isError(remaining) != 0) {
            fail_zstd_encoding(*this, remaining);
        }
        if (probing && output_buffer.pos != 0) {
            return false;
        }
        output.append(buffer.data(), output_buffer.pos);
        const bool input_consumed = input_buffer.pos == input_buffer.size;
        if (input_consumed && (operation == ZSTD_e_continue ? output_buffer.pos < writable : remaining == 0)) {
            return true;
        }
        if (probing) {
            return false;
        }
        if (output_buffer.pos == 0 && input_buffer.pos == before_input) {
            throw http_content_codec_error("Zstd content encoder made no progress");
        }
    }
}

std::variant<std::string, http_content_decode_error> decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes) {
    std::string output;
    zstd_decoder_stream decoder;
    if (!decoder.initialized()) {
        throw http_content_codec_error("zstd decoder initialization failed");
    }
    auto* stream = decoder.native();

    ZSTD_inBuffer in{input.data(), input.size(), 0};
    char buffer[16384];
    for (;;) {
        const auto before_input = in.pos;
        ZSTD_outBuffer out{buffer, sizeof(buffer), 0};
        const auto result_value = ZSTD_decompressStream(stream, &out, &in);
        if (ZSTD_isError(result_value) != 0) {
            decoder.rethrow_allocation_failure();
            if (ZSTD_getErrorCode(result_value) == ZSTD_error_memory_allocation) {
                throw http_content_codec_error(ZSTD_getErrorName(result_value));
            }
            return http_content_decode_error::invalid_content;
        }
        if (!append_decoded_bytes(output, buffer, out.pos, max_decoded_bytes)) {
            return http_content_decode_error::decoded_size_exceeded;
        }
        if (result_value == 0 && in.pos == in.size) {
            return output;
        }
        // A zero result with remaining input completed one RFC 8878 frame;
        // ZSTD_decompressStream is ready to consume the next concatenated frame.
        if (out.pos == 0 && in.pos == before_input) {
            return http_content_decode_error::invalid_content;
        }
    }
}

std::optional<std::string> encode_zstd_content(
    zstd_encoder_stream& encoder, std::string_view input,
    std::size_t max_encoded_bytes) {
    return encode_whole_content_with_stream(encoder, ZSTD_e_end, input, max_encoded_bytes);
}

std::optional<std::string> encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content<zstd_encoder_stream>(
        "zstd encoder initialization failed", ZSTD_e_end, input, max_encoded_bytes);
}

}  // namespace ruvia::detail

namespace ruvia {

namespace {

using encoder_stream = std::variant<detail::zlib_deflate_stream, detail::brotli_encoder_stream, detail::zstd_encoder_stream>;

void emplace_encoder_stream(std::optional<encoder_stream>& target, http_content_coding coding,
    std::size_t max_memory_bytes) {
    switch (coding) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
            target.emplace(std::in_place_type<detail::zlib_deflate_stream>,
                coding == http_content_coding::gzip ? detail::zlib_content_format::gzip
                                                    : detail::zlib_content_format::deflate,
                max_memory_bytes);
            return;
        case http_content_coding::brotli:
            target.emplace(std::in_place_type<detail::brotli_encoder_stream>,
                detail::brotli_stream_window_bits, max_memory_bytes);
            return;
        case http_content_coding::zstd:
            target.emplace(std::in_place_type<detail::zstd_encoder_stream>, max_memory_bytes);
            return;
        case http_content_coding::identity:
            break;
    }
    throw std::invalid_argument("unsupported HTTP content coding");
}

[[nodiscard]] const char* initialization_failure(const detail::zlib_deflate_stream&) noexcept {
    return "failed to initialize zlib content encoder";
}

[[nodiscard]] const char* initialization_failure(const detail::brotli_encoder_stream&) noexcept {
    return "failed to initialize Brotli content encoder";
}

[[nodiscard]] const char* initialization_failure(const detail::zstd_encoder_stream&) noexcept {
    return "failed to initialize Zstd content encoder";
}

[[nodiscard]] bool encode_stream(detail::zlib_deflate_stream& deflater, std::string_view input,
    std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const int operation = finish_value ? Z_FINISH : flush ? Z_SYNC_FLUSH
                                                          : Z_NO_FLUSH;
    return deflater.encode(input, output, operation, max_encoded_bytes);
}

[[nodiscard]] bool encode_stream(detail::brotli_encoder_stream& encoder, std::string_view input,
    std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const auto operation = finish_value ? BROTLI_OPERATION_FINISH : flush ? BROTLI_OPERATION_FLUSH
                                                                          : BROTLI_OPERATION_PROCESS;
    return encoder.encode(input, output, operation, max_encoded_bytes);
}

[[nodiscard]] bool encode_stream(detail::zstd_encoder_stream& encoder, std::string_view input,
    std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const auto operation = finish_value ? ZSTD_e_end : flush ? ZSTD_e_flush
                                                             : ZSTD_e_continue;
    return encoder.encode(input, output, operation, max_encoded_bytes);
}

}  // namespace

// One pinned codec stream per non-identity encoder. A failed initialization
// unwinds through the stream destructor, including an exception captured at
// the C ABI.
struct http_content_encoder::impl_type final {
    impl_type(http_content_coding coding, http_content_encoder_options options)
        : max_encoded_bytes_(options.max_encoded_bytes_),
          max_memory_bytes_(options.max_memory_bytes_),
          coding_(coding) {
        if (coding_ != http_content_coding::identity) {
            emplace_encoder_stream(stream_, coding_, max_memory_bytes_);
            std::visit(
                [](const auto& stream) {
                    if (!stream.initialized()) {
                        throw http_content_codec_error(initialization_failure(stream));
                    }
                },
                *stream_);
        }
    }

    void encode(std::string_view input, std::string& output, bool flush, bool finish_value) {
        const auto before_size = output.size();
        if (!stream_.has_value()) {
            if (!input.empty()) {
                if (input.size() > max_encoded_bytes_ - produced_bytes_) {
                    throw http_content_codec_error("encoded HTTP content size exceeded limit");
                }
                output.append(input.data(), input.size());
                produced_bytes_ += input.size();
            }
            return;
        }
        const auto remaining = max_encoded_bytes_ - produced_bytes_;
        const auto output_cap = remaining > (std::numeric_limits<std::size_t>::max)() - before_size
                                    ? (std::numeric_limits<std::size_t>::max)()
                                    : before_size + remaining;
        const bool ok = std::visit(
            [&](auto& stream) {
                return encode_stream(stream, input, output, flush, finish_value, output_cap);
            },
            *stream_);
        if (!ok) {
            throw http_content_codec_error("encoded HTTP content size exceeded limit");
        }
        produced_bytes_ += output.size() - before_size;
    }

    // Reuse native working memory where supported. Brotli rebuilds.
    // A failed rebuild leaves the variant valueless, which the
    // owner never touches again until another explicit reset().
    void reset() {
        produced_bytes_ = 0;
        if (!stream_.has_value()) {
            return;
        }
        if (auto* deflater = std::get_if<detail::zlib_deflate_stream>(&*stream_)) {
            if (!deflater->reset()) {
                throw http_content_codec_error("failed to reset zlib content encoder");
            }
        } else if (auto* encoder = std::get_if<detail::zstd_encoder_stream>(&*stream_)) {
            if (!encoder->reset()) {
                throw http_content_codec_error("failed to reset Zstd content encoder");
            }
        } else {
            const auto& rebuilt = stream_->emplace<detail::brotli_encoder_stream>(
                detail::brotli_stream_window_bits, max_memory_bytes_);
            if (!rebuilt.initialized()) {
                throw http_content_codec_error(initialization_failure(rebuilt));
            }
        }
    }

    std::optional<encoder_stream> stream_;
    std::size_t max_encoded_bytes_;
    std::size_t max_memory_bytes_;
    std::size_t produced_bytes_{0};
    http_content_coding coding_;
};

http_content_encoder::http_content_encoder(
    http_content_coding coding, http_content_encoder_options options)
    : coding_(coding) {
    if (coding_ != http_content_coding::identity ||
        options.max_encoded_bytes_ != (std::numeric_limits<std::size_t>::max)() ||
        options.max_memory_bytes_ != (std::numeric_limits<std::size_t>::max)()) {
        if (http_content_coding_token(coding_).empty()) {
            throw std::invalid_argument("unsupported HTTP content coding");
        }
        impl_ = new impl_type(coding_, options);
    }
}

http_content_encoder::~http_content_encoder() {
    delete impl_;
}

void http_content_encoder::write(std::string_view input, std::string& output, bool flush) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP content encoder is no longer writable");
    }
    try {
        if (impl_ == nullptr) {
            if (!input.empty()) {
                output.append(input.data(), input.size());
            }
        } else if (!input.empty() || flush) {
            if (detail::http_text_aliases_string(input, output)) {
                // Appending codec output can reallocate the caller's string
                // before all input has been consumed. Own only this aliasing
                // case; independent input keeps the direct encoding path.
                const std::string owned_input(input);
                impl_->encode(owned_input, output, flush, false);
            } else {
                impl_->encode(input, output, flush, false);
            }
        }
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_encoder::finish(std::string& output) {
    if (phase_ == phase::finished) {
        return;
    }
    if (phase_ == phase::failed) {
        throw std::logic_error("HTTP content encoder has failed");
    }
    try {
        if (impl_ != nullptr) {
            impl_->encode({}, output, false, true);
        }
        phase_ = phase::finished;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_encoder::reset() {
    if (impl_ != nullptr) {
        try {
            impl_->reset();
        } catch (...) {
            phase_ = phase::failed;
            throw;
        }
    }
    phase_ = phase::active;
}
// The kept state of a resettable coding. The native libraries retain its
// address, so it lives in one stable allocation.
struct http_content_encode_context::impl_type final {
    using stream_type = std::variant<detail::zlib_deflate_stream, detail::zstd_encoder_stream>;

    explicit impl_type(http_content_coding coding)
        : stream_(make_stream(coding)) {}

    [[nodiscard]] bool initialized() const noexcept {
        return std::visit([](const auto& stream) { return stream.initialized(); }, stream_);
    }

    [[nodiscard]] bool reset() noexcept {
        return std::visit([](auto& stream) { return stream.reset(); }, stream_);
    }

    [[nodiscard]] std::optional<std::string> encode(
        std::string_view input, http_content_encode_options options) {
        if (auto* deflater = std::get_if<detail::zlib_deflate_stream>(&stream_)) {
            return detail::encode_zlib_content(*deflater, input, options.max_encoded_bytes_);
        }
        return detail::encode_zstd_content(
            std::get<detail::zstd_encoder_stream>(stream_), input, options.max_encoded_bytes_);
    }

    stream_type stream_;

private:
    [[nodiscard]] static stream_type make_stream(http_content_coding coding) {
        if (coding == http_content_coding::zstd) {
            return stream_type(std::in_place_type<detail::zstd_encoder_stream>);
        }
        return stream_type(std::in_place_type<detail::zlib_deflate_stream>,
            coding == http_content_coding::gzip ? detail::zlib_content_format::gzip
                                                : detail::zlib_content_format::deflate);
    }
};

http_content_encode_context::http_content_encode_context(
    http_content_coding coding) noexcept
    : coding_(coding) {}

http_content_encode_context::~http_content_encode_context() {
    delete impl_;
}

std::optional<std::string> http_content_encode_context::encode(
    std::string_view input, http_content_encode_options options) {
    switch (coding_) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
        case http_content_coding::zstd:
            break;
        case http_content_coding::brotli: {
            detail::brotli_encoder_stream encoder(detail::brotli_whole_buffer_window_bits);
            if (!encoder.initialized()) {
                throw http_content_codec_error("brotli encoder initialization failed");
            }
            return detail::encode_brotli_content(encoder, input, options.max_encoded_bytes_);
        }
        default:
            // identity and unknown values keep the one-shot facade's contract.
            return encode_http_content(coding_, input, options);
    }

    if (detail::acquire_codec(impl_, coding_) == nullptr) {
        throw http_content_codec_error("content encoder initialization failed");
    }
    const detail::codec_reset_guard reset_guard(impl_);
    return impl_->encode(input, options);
}

namespace {

class streaming_decode_stage final {
public:
    streaming_decode_stage(http_content_coding coding,
        std::size_t max_decoded_bytes, std::size_t max_memory_bytes)
        : coding_(coding),
          max_decoded_bytes_(max_decoded_bytes) {
        emplace_decoder_stream(coding, max_decoded_bytes, max_memory_bytes);
    }
    streaming_decode_stage(const streaming_decode_stage&) = delete;
    streaming_decode_stage& operator=(const streaming_decode_stage&) = delete;

    void write(std::string_view input, std::string& output) {
        if (input.empty()) {
            return;
        }
        saw_input_ = true;
        if (auto* zlib_decoder = std::get_if<detail::zlib_inflate_stream>(&*stream_)) {
            write_zlib(*zlib_decoder, input, output);
        } else if (auto* brotli_decoder = std::get_if<detail::brotli_decoder_stream>(&*stream_)) {
            write_brotli(*brotli_decoder, input, output);
        } else if (auto* zstd_decoder = std::get_if<detail::zstd_decoder_stream>(&*stream_)) {
            write_zstd(*zstd_decoder, input, output);
        }
    }

    void finish() const {
        if (saw_input_ && !at_frame_end_) {
            throw http_content_transcode_error(
                http_content_decode_error::invalid_content, "truncated compressed HTTP content");
        }
    }

private:
    using decoder_variant = std::variant<detail::zlib_inflate_stream, detail::brotli_decoder_stream,
        detail::zstd_decoder_stream>;

    void emplace_decoder_stream(http_content_coding coding,
        std::size_t max_decoded_bytes, std::size_t max_memory_bytes) {
        switch (coding) {
            case http_content_coding::gzip:
            case http_content_coding::deflate: {
                auto& var = stream_.emplace(std::in_place_type<detail::zlib_inflate_stream>,
                    coding == http_content_coding::gzip ? detail::zlib_gzip_window_bits
                                                        : detail::zlib_deflate_window_bits,
                    max_memory_bytes);
                if (!std::get<detail::zlib_inflate_stream>(var).initialized()) {
                    throw http_content_codec_error("failed to initialize zlib content decoder");
                }
                return;
            }
            case http_content_coding::brotli: {
                auto& var = stream_.emplace(std::in_place_type<detail::brotli_decoder_stream>,
                    max_decoded_bytes, max_memory_bytes);
                const auto& decoder = std::get<detail::brotli_decoder_stream>(var);
                if (!decoder.initialized()) {
                    if (decoder.native_budget_exceeded()) {
                        throw http_content_transcode_error(
                            http_content_decode_error::decoded_size_exceeded,
                            "Brotli decoder memory budget exceeded");
                    }
                    throw http_content_codec_error("failed to initialize Brotli content decoder");
                }
                return;
            }
            case http_content_coding::zstd: {
                auto& var =
                    stream_.emplace(std::in_place_type<detail::zstd_decoder_stream>, max_memory_bytes);
                if (!std::get<detail::zstd_decoder_stream>(var).initialized()) {
                    throw http_content_codec_error("failed to initialize Zstd content decoder");
                }
                return;
            }
            case http_content_coding::identity:
                break;
        }
        throw std::invalid_argument("unsupported HTTP content coding for decode stage");
    }

    void append_checked(std::string& output, const char* data, std::size_t size) {
        if (size > max_decoded_bytes_ - decoded_bytes_) {
            throw http_content_transcode_error(
                http_content_decode_error::decoded_size_exceeded,
                "decoded HTTP content size exceeded limit");
        }
        output.append(data, size);
        decoded_bytes_ += size;
    }

    void write_zlib(
        detail::zlib_inflate_stream& inflater, std::string_view input, std::string& output) {
        const bool allow_concatenated = coding_ == http_content_coding::gzip;
        const int window_bits =
            allow_concatenated ? detail::zlib_gzip_window_bits : detail::zlib_deflate_window_bits;
        auto& stream = inflater.native();
        const detail::zlib_borrow_scope borrow(stream);
        if (at_frame_end_) {
            if (!allow_concatenated) {
                throw http_content_transcode_error(http_content_decode_error::invalid_content,
                    "trailing bytes after deflate stream");
            }
            if (inflateReset2(&stream, window_bits) != Z_OK) {
                throw http_content_codec_error("failed to reset zlib content decoder");
            }
            at_frame_end_ = false;
        }
        std::size_t supplied = 0;
        std::array<char, 16384> buffer;
        for (;;) {
            detail::zlib_refill_input(stream, input, supplied);
            const auto before_input = stream.avail_in;
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            const int status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
                inflater.rethrow_allocation_failure();
            }
            const auto produced = buffer.size() - stream.avail_out;
            if (produced != 0) {
                append_checked(output, buffer.data(), produced);
            }
            if (status == Z_STREAM_END) {
                at_frame_end_ = true;
                detail::zlib_refill_input(stream, input, supplied);
                if (stream.avail_in == 0 && supplied == input.size()) {
                    return;
                }
                if (!allow_concatenated) {
                    throw http_content_transcode_error(http_content_decode_error::invalid_content,
                        "trailing bytes after deflate stream");
                }
                auto* const next_input = stream.next_in;
                const auto available_input = stream.avail_in;
                if (inflateReset2(&stream, window_bits) != Z_OK) {
                    throw http_content_codec_error("failed to reset zlib content decoder");
                }
                stream.next_in = next_input;
                stream.avail_in = available_input;
                at_frame_end_ = false;
                continue;
            }
            if (status == Z_MEM_ERROR) {
                throw http_content_codec_error("zlib decoder ran out of memory");
            }
            if (status != Z_OK && status != Z_BUF_ERROR) {
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid zlib compressed content");
            }
            const bool progressed = produced != 0 || stream.avail_in != before_input;
            if (!progressed) {
                if (stream.avail_in == 0 && supplied < input.size()) {
                    continue;
                }
                if (stream.avail_in == 0 && supplied == input.size()) {
                    return;
                }
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid zlib compressed content");
            }
        }
    }

    void write_brotli(
        detail::brotli_decoder_stream& decoder, std::string_view input, std::string& output) {
        if (at_frame_end_) {
            throw http_content_transcode_error(
                http_content_decode_error::invalid_content, "trailing bytes after Brotli stream");
        }
        auto* const state_value = decoder.native();
        const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
        std::size_t available_input = input.size();
        for (;;) {
            const auto before_input = available_input;
            std::size_t available_output = 0;
            const auto result_value = BrotliDecoderDecompressStream(
                state_value, &available_input, &next_input, &available_output, nullptr, nullptr);
            bool produced_output = false;
            while (BrotliDecoderHasMoreOutput(state_value) == BROTLI_TRUE) {
                std::size_t produced = 0;
                const auto* const bytes_value = BrotliDecoderTakeOutput(state_value, &produced);
                if (produced != 0) {
                    append_checked(output, reinterpret_cast<const char*>(bytes_value), produced);
                    produced_output = true;
                }
            }
            if (result_value == BROTLI_DECODER_RESULT_SUCCESS) {
                if (available_input != 0) {
                    throw http_content_transcode_error(http_content_decode_error::invalid_content,
                        "trailing bytes after Brotli stream");
                }
                at_frame_end_ = true;
                return;
            }
            if (result_value == BROTLI_DECODER_RESULT_ERROR) {
                if (decoder.native_budget_exceeded()) {
                    throw http_content_transcode_error(
                        http_content_decode_error::decoded_size_exceeded,
                        "Brotli decoder memory budget exceeded");
                }
                decoder.rethrow_allocation_failure();
                throw http_content_transcode_error(http_content_decode_error::invalid_content,
                    "invalid Brotli compressed content");
            }
            if (result_value == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && available_input == 0) {
                return;
            }
            if (!produced_output && available_input == before_input) {
                throw http_content_transcode_error(http_content_decode_error::invalid_content,
                    "invalid Brotli compressed content");
            }
        }
    }

    void write_zstd(
        detail::zstd_decoder_stream& decoder, std::string_view input, std::string& output) {
        auto* const stream = decoder.native();
        ZSTD_inBuffer in{input.data(), input.size(), 0};
        std::array<char, 16384> buffer;
        at_frame_end_ = false;
        for (;;) {
            const auto before_input = in.pos;
            ZSTD_outBuffer out{buffer.data(), buffer.size(), 0};
            const auto result_value = ZSTD_decompressStream(stream, &out, &in);
            if (ZSTD_isError(result_value) != 0) {
                decoder.rethrow_allocation_failure();
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid Zstd compressed content");
            }
            if (out.pos != 0) {
                append_checked(output, buffer.data(), out.pos);
            }
            if (result_value == 0) {
                at_frame_end_ = true;
                if (in.pos == in.size) {
                    return;
                }
                at_frame_end_ = false;
                continue;
            }
            if (in.pos == in.size && out.pos < buffer.size()) {
                return;
            }
            if (out.pos == 0 && in.pos == before_input) {
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid Zstd compressed content");
            }
        }
    }

    http_content_coding coding_;
    std::size_t max_decoded_bytes_;
    std::size_t decoded_bytes_{0};
    bool saw_input_{false};
    bool at_frame_end_{false};
    std::optional<decoder_variant> stream_;
};

}  // namespace

struct http_content_transcoder::impl_type final {
    using stage_ptr = std::unique_ptr<streaming_decode_stage>;

    impl_type(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_content_transcoder_options options)
        : options_(options),
          target_coding_(target_coding),
          encoder_(target_coding,
              http_content_encoder_options{
                  .max_encoded_bytes_ = options.max_encoded_bytes_,
                  .max_memory_bytes_ = options.max_memory_bytes_}) {
        if (http_content_coding_token(target_coding).empty()) {
            throw std::invalid_argument("unsupported target HTTP content coding");
        }
        for (auto it = source_codings.rbegin(); it != source_codings.rend(); ++it) {
            if (http_content_coding_token(*it).empty()) {
                throw std::invalid_argument("unsupported source HTTP content coding");
            }
            if (*it != http_content_coding::identity) {
                source_codings_.push_back(*it);
            }
        }
        build_stages();
    }

    void build_stages() {
        stages_.clear();
        stages_.reserve(source_codings_.size());
        for (const auto coding : source_codings_) {
            stages_.push_back(std::make_unique<streaming_decode_stage>(
                coding, options_.max_decoded_bytes_, options_.max_memory_bytes_));
        }
        identity_decoded_bytes_ = 0;
    }

    void write(std::string_view input, std::string& output, bool flush) {
        if (stages_.empty()) {
            if (input.size() > options_.max_decoded_bytes_ - identity_decoded_bytes_) {
                throw http_content_transcode_error(
                    http_content_decode_error::decoded_size_exceeded,
                    "decoded HTTP content size exceeded limit");
            }
            identity_decoded_bytes_ += input.size();
            encoder_.write(input, output, flush);
            return;
        }
        if (stages_.size() == 1 && target_coding_ == http_content_coding::identity &&
            options_.max_encoded_bytes_ == (std::numeric_limits<std::size_t>::max)() &&
            !detail::http_text_aliases_string(input, output)) {
            stages_.front()->write(input, output);
            return;
        }
        scratch_a_.clear();
        stages_.front()->write(input, scratch_a_);
        std::string* current = &scratch_a_;
        std::string* next = &scratch_b_;
        for (std::size_t index = 1; index < stages_.size(); ++index) {
            next->clear();
            stages_[index]->write(*current, *next);
            std::swap(current, next);
        }
        if (!current->empty() || flush) {
            encoder_.write(*current, output, flush);
        }
    }

    void finish(std::string& output) {
        for (const auto& stage : stages_) {
            stage->finish();
        }
        encoder_.finish(output);
    }

    void reset() {
        build_stages();
        scratch_a_.clear();
        scratch_b_.clear();
        encoder_.reset();
    }

    std::vector<http_content_coding> source_codings_;
    std::vector<stage_ptr> stages_;
    std::string scratch_a_;
    std::string scratch_b_;
    http_content_transcoder_options options_;
    std::size_t identity_decoded_bytes_{0};
    http_content_coding target_coding_;
    http_content_encoder encoder_;
};

http_content_transcoder::http_content_transcoder(http_content_coding source_coding,
    http_content_coding target_coding, http_content_transcoder_options options)
    : http_content_transcoder(
          std::span<const http_content_coding>(&source_coding, 1), target_coding, options) {}

http_content_transcoder::http_content_transcoder(
    std::span<const http_content_coding> source_codings, http_content_coding target_coding,
    http_content_transcoder_options options)
    : target_coding_(target_coding),
      impl_(new impl_type(source_codings, target_coding, options)) {}

http_content_transcoder::~http_content_transcoder() {
    delete impl_;
}

void http_content_transcoder::write(
    std::string_view input, std::string& output, bool flush) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP content transcoder is no longer writable");
    }
    try {
        impl_->write(input, output, flush);
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_transcoder::finish(std::string& output) {
    if (phase_ == phase::finished) {
        return;
    }
    if (phase_ == phase::failed) {
        throw std::logic_error("HTTP content transcoder has failed");
    }
    try {
        impl_->finish(output);
        phase_ = phase::finished;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_transcoder::reset() {
    try {
        impl_->reset();
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
    phase_ = phase::active;
}

struct http_staged_content_transcoder::impl_type final {
    impl_type(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_staged_transcode_options options)
        : min_bytes_(options.min_bytes_),
          max_bytes_(options.max_bytes_),
          max_memory_bytes_(options.max_memory_bytes_),
          target_coding_(target_coding),
          require_smaller_(options.require_smaller_than_source_) {
        if (min_bytes_ > max_bytes_) {
            throw std::invalid_argument(
                "staged transcoder minimum size must not exceed maximum size");
        }
        if (http_content_coding_token(target_coding_).empty()) {
            throw std::invalid_argument("unsupported HTTP target content coding");
        }
        for (const auto coding : source_codings) {
            if (http_content_coding_token(coding).empty()) {
                throw std::invalid_argument("unsupported HTTP source content coding");
            }
            if (coding != http_content_coding::identity) {
                source_codings_.push_back(coding);
            }
        }
        if (!source_codings_.empty() && max_bytes_ > 0) {
            const std::size_t decode_cap =
                max_bytes_ == (std::numeric_limits<std::size_t>::max)() ? max_bytes_ : max_bytes_ + 1U;
            decoder_.emplace(source_codings_, http_content_coding::identity,
                http_content_transcoder_options{
                    .max_decoded_bytes_ = decode_cap,
                    .max_memory_bytes_ = max_memory_bytes_,
                });
        }
    }

    void switch_to_passthrough() noexcept {
        passthrough_ = true;
        decoder_.reset();
        std::string().swap(staged_decoded_);
    }

    bool write(std::string_view input, std::string* passthrough_out) {
        if (passthrough_) {
            if (!input.empty()) {
                if (passthrough_out != nullptr) {
                    if (!staged_raw_.empty()) {
                        passthrough_out->append(staged_raw_);
                        std::string().swap(staged_raw_);
                    }
                    passthrough_out->append(input);
                } else {
                    staged_raw_.append(input);
                }
            } else if (passthrough_out != nullptr && !staged_raw_.empty()) {
                passthrough_out->append(staged_raw_);
                std::string().swap(staged_raw_);
            }
            return false;
        }

        if (input.size() > max_bytes_ || staged_raw_.size() > max_bytes_ - input.size()) {
            switch_to_passthrough();
            if (passthrough_out != nullptr) {
                if (!staged_raw_.empty()) {
                    passthrough_out->append(staged_raw_);
                    std::string().swap(staged_raw_);
                }
                passthrough_out->append(input);
            } else {
                staged_raw_.append(input);
            }
            return false;
        }

        if (input.size() > max_memory_bytes_ - staged_raw_.size()) {
            throw std::bad_alloc();
        }
        staged_raw_.append(input);
        if (source_codings_.empty()) {
            decoded_bytes_ = staged_raw_.size();
            return true;
        }

        try {
            decoder_->write(input, staged_decoded_, false);
            decoded_bytes_ = staged_decoded_.size();
            if (decoded_bytes_ > max_bytes_) {
                switch_to_passthrough();
                if (passthrough_out != nullptr && !staged_raw_.empty()) {
                    passthrough_out->append(staged_raw_);
                    std::string().swap(staged_raw_);
                }
                return false;
            }
        } catch (const http_content_transcode_error& err) {
            if (err.decode_error() == http_content_decode_error::decoded_size_exceeded) {
                switch_to_passthrough();
                if (passthrough_out != nullptr && !staged_raw_.empty()) {
                    passthrough_out->append(staged_raw_);
                    std::string().swap(staged_raw_);
                }
                return false;
            }
            throw;
        }
        return true;
    }

    http_staged_transcode_result finish() {
        const http_content_coding fallback_coding =
            source_codings_.empty() ? http_content_coding::identity : source_codings_.back();
        if (passthrough_) {
            return http_staged_transcode_result(
                false, fallback_coding, decoded_bytes_, std::move(staged_raw_));
        }
        if (!source_codings_.empty()) {
            try {
                decoder_->finish(staged_decoded_);
                decoded_bytes_ = staged_decoded_.size();
                if (decoded_bytes_ > max_bytes_) {
                    switch_to_passthrough();
                    return http_staged_transcode_result(
                        false, fallback_coding, decoded_bytes_, std::move(staged_raw_));
                }
            } catch (const http_content_transcode_error& err) {
                if (err.decode_error() == http_content_decode_error::decoded_size_exceeded) {
                    switch_to_passthrough();
                    return http_staged_transcode_result(
                        false, fallback_coding, decoded_bytes_, std::move(staged_raw_));
                }
                throw;
            }
        } else {
            decoded_bytes_ = staged_raw_.size();
        }

        const std::string_view plain_view =
            source_codings_.empty() ? std::string_view(staged_raw_) : std::string_view(staged_decoded_);
        if (plain_view.size() < min_bytes_ || plain_view.size() > max_bytes_ ||
            (source_codings_.empty() && target_coding_ == http_content_coding::identity)) {
            return http_staged_transcode_result(
                false, fallback_coding, plain_view.size(), std::move(staged_raw_));
        }
        if (target_coding_ == http_content_coding::identity) {
            return http_staged_transcode_result(true, http_content_coding::identity,
                plain_view.size(), std::move(staged_decoded_));
        }

        const std::size_t max_encoded =
            require_smaller_
                ? (staged_raw_.empty() ? 0U : staged_raw_.size() - 1U)
                : (std::numeric_limits<std::size_t>::max)();
        std::optional<std::string> encoded;
        if (max_memory_bytes_ == (std::numeric_limits<std::size_t>::max)()) {
            encoded = encode_http_content(target_coding_, plain_view,
                http_content_encode_options{
                    .max_encoded_bytes_ = max_encoded,
                });
        } else {
            switch (target_coding_) {
                case http_content_coding::gzip:
                case http_content_coding::deflate: {
                    detail::zlib_deflate_stream deflater(
                        target_coding_ == http_content_coding::gzip
                            ? detail::zlib_content_format::gzip
                            : detail::zlib_content_format::deflate,
                        max_memory_bytes_);
                    if (!deflater.initialized()) {
                        throw http_content_codec_error("zlib encoder initialization failed");
                    }
                    encoded = detail::encode_zlib_content(deflater, plain_view, max_encoded);
                    break;
                }
                case http_content_coding::brotli: {
                    detail::brotli_encoder_stream encoder(
                        detail::brotli_whole_buffer_window_bits, max_memory_bytes_);
                    if (!encoder.initialized()) {
                        throw http_content_codec_error("brotli encoder initialization failed");
                    }
                    encoded = detail::encode_brotli_content(encoder, plain_view, max_encoded);
                    break;
                }
                case http_content_coding::zstd: {
                    detail::zstd_encoder_stream encoder(max_memory_bytes_);
                    if (!encoder.initialized()) {
                        throw http_content_codec_error("zstd encoder initialization failed");
                    }
                    encoded = detail::encode_zstd_content(encoder, plain_view, max_encoded);
                    break;
                }
                case http_content_coding::identity:
                    break;
            }
        }
        if (!encoded.has_value()) {
            return http_staged_transcode_result(
                false, fallback_coding, plain_view.size(), std::move(staged_raw_));
        }
        return http_staged_transcode_result(
            true, target_coding_, plain_view.size(), std::move(*encoded));
    }

    void reset() {
        staged_raw_.clear();
        staged_decoded_.clear();
        decoded_bytes_ = 0;
        passthrough_ = false;
        if (!source_codings_.empty() && max_bytes_ > 0) {
            const std::size_t decode_cap =
                max_bytes_ == (std::numeric_limits<std::size_t>::max)() ? max_bytes_ : max_bytes_ + 1U;
            if (decoder_.has_value()) {
                decoder_->reset();
            } else {
                decoder_.emplace(source_codings_, http_content_coding::identity,
                    http_content_transcoder_options{
                        .max_decoded_bytes_ = decode_cap,
                        .max_memory_bytes_ = max_memory_bytes_,
                    });
            }
        } else {
            decoder_.reset();
        }
    }

    std::vector<http_content_coding> source_codings_;
    std::optional<http_content_transcoder> decoder_;
    std::string staged_raw_;
    std::string staged_decoded_;
    std::size_t min_bytes_;
    std::size_t max_bytes_;
    std::size_t max_memory_bytes_;
    std::size_t decoded_bytes_{0};
    http_content_coding target_coding_;
    bool require_smaller_{false};
    bool passthrough_{false};
};

http_staged_content_transcoder::http_staged_content_transcoder(
    http_content_coding source_coding, http_content_coding target_coding,
    http_staged_transcode_options options)
    : http_staged_content_transcoder(
          std::span<const http_content_coding>(&source_coding, 1), target_coding, options) {}

http_staged_content_transcoder::http_staged_content_transcoder(
    std::span<const http_content_coding> source_codings, http_content_coding target_coding,
    http_staged_transcode_options options)
    : target_coding_(target_coding),
      impl_(new impl_type(source_codings, target_coding, options)) {}

http_staged_content_transcoder::~http_staged_content_transcoder() {
    delete impl_;
}

bool http_staged_content_transcoder::passthrough() const noexcept {
    return impl_ != nullptr && impl_->passthrough_;
}

std::size_t http_staged_content_transcoder::staged_raw_bytes() const noexcept {
    return impl_ != nullptr ? impl_->staged_raw_.size() : 0;
}

std::size_t http_staged_content_transcoder::decoded_bytes() const noexcept {
    return impl_ != nullptr ? impl_->decoded_bytes_ : 0;
}

bool http_staged_content_transcoder::write(std::string_view input) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder is no longer writable");
    }
    try {
        return impl_->write(input, nullptr);
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

bool http_staged_content_transcoder::write(
    std::string_view input, std::string& passthrough_output) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder is no longer writable");
    }
    try {
        if (detail::http_text_aliases_string(input, passthrough_output)) {
            const std::string owned(input);
            return impl_->write(owned, &passthrough_output);
        }
        return impl_->write(input, &passthrough_output);
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

http_staged_transcode_result http_staged_content_transcoder::finish() {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder cannot finish in current state");
    }
    try {
        auto result = impl_->finish();
        phase_ = phase::finished;
        return result;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_staged_content_transcoder::reset() {
    try {
        impl_->reset();
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
    phase_ = phase::active;
}

http_worker_codecs::http_worker_codecs() noexcept
    : gzip_(http_content_coding::gzip),
      deflate_(http_content_coding::deflate),
      brotli_(http_content_coding::brotli),
      zstd_(http_content_coding::zstd) {}

http_worker_codecs::~http_worker_codecs() {
    for (auto* pool : {&gzip_.streaming_, &deflate_.streaming_, &brotli_.streaming_, &zstd_.streaming_}) {
        for (std::size_t i = 0; i < pool->count_; ++i) {
            destroy_stream_encoder(*pool->free_[i]);
        }
        pool->count_ = 0;
    }
}

http_content_encode_context& http_worker_codecs::encoder(http_content_coding coding) {
    return encoders(coding).buffered_;
}

http_worker_codecs::coding_encoders_type& http_worker_codecs::encoders(http_content_coding coding) {
    switch (coding) {
        case http_content_coding::gzip:
            return gzip_;
        case http_content_coding::deflate:
            return deflate_;
        case http_content_coding::brotli:
            return brotli_;
        case http_content_coding::zstd:
            return zstd_;
        case http_content_coding::identity:
            break;
    }
    throw std::logic_error("identity content coding has no encoder");
}

http_stream_encoder_lease http_worker_codecs::acquire_stream_encoder(http_content_coding coding) {
    auto& pool = encoders(coding).streaming_;
    if (pool.count_ != 0) {
        return http_stream_encoder_lease(*this, *pool.free_[--pool.count_]);
    }
    return http_stream_encoder_lease(*this, *(new http_content_encoder(coding)));
}

void http_worker_codecs::release_stream_encoder(http_content_encoder& encoder) noexcept {
    auto& pool = encoders(encoder.coding()).streaming_;
    if (pool.count_ < pool.free_.size()) {
        try {
            encoder.reset();
            pool.free_[pool.count_++] = &encoder;
            return;
        } catch (...) {
        }
    }
    destroy_stream_encoder(encoder);
}

void http_worker_codecs::destroy_stream_encoder(http_content_encoder& encoder) noexcept {
    delete &encoder;
}
}  // namespace ruvia

// The MIME entity grammar multipart bodies are validated against (RFC 2045
// sections 5.1 and 5.3): which characters form a token, what a field name, field
// body, parameter and media type may contain, and the duplicate-parameter guard
// a media type needs.

namespace ruvia::detail {

[[nodiscard]] inline bool http_mime_token_char(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    if (byte <= 0x20 || byte >= 0x7F) {
        return false;
    }
    switch (value) {
        case '(':
        case ')':
        case '<':
        case '>':
        case '@':
        case ',':
        case ';':
        case ':':
        case '\\':
        case '"':
        case '/':
        case '[':
        case ']':
        case '?':
        case '=':
            return false;
        default:
            return true;
    }
}

[[nodiscard]] inline bool http_valid_mime_field_name(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::ranges::all_of(value, [](char byte) {
        const auto character = static_cast<unsigned char>(byte);
        return character >= 33 && character <= 126 && byte != ':';
    });
}

[[nodiscard]] inline bool http_valid_mime_parameter_value(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        return std::ranges::all_of(value, http_mime_token_char);
    }
    return http_is_quoted_string(value);
}

[[nodiscard]] inline bool http_parse_mime_parameter(std::string_view parameter, std::string_view& name,
    std::string_view& value, bool strict_equals = true) noexcept {
    const auto equals = parameter.find('=');
    if (parameter.empty() || equals == std::string_view::npos) {
        return false;
    }

    const auto raw_name = parameter.substr(0, equals);
    const auto raw_value = parameter.substr(equals + 1);
    name = http_trim_ows(raw_name);
    value = http_trim_ows(raw_value);
    // Top-level HTTP media-type parameters forbid OWS around '=' (RFC 9110
    // section 5.6.6). MIME body-part structured fields retain RFC 822's
    // separator whitespace and opt out while sharing the remaining checks.
    return (!strict_equals || (name.size() == raw_name.size() && value.size() == raw_value.size())) &&
           !name.empty() && std::ranges::all_of(name, http_mime_token_char) &&
           http_valid_mime_parameter_value(value);
}

template <http_temporary_owning_char_string parameter_type>
bool http_parse_mime_parameter(
    parameter_type&&, std::string_view&, std::string_view&, bool = true) = delete;

class http_mime_parameter_names final {
public:
    [[nodiscard]] bool record(std::string_view name) noexcept {
        for (std::size_t index = 0; index < size_; ++index) {
            if (http_ascii_equals_ignore_case(names_[index], name)) {
                return false;
            }
        }
        // Parameter-heavy input is hostile in practice. A fixed bound keeps
        // duplicate detection allocation-free and its worst-case work constant.
        if (size_ == names_.size()) {
            return false;
        }
        names_[size_++] = name;
        return true;
    }

private:
    std::array<std::string_view, 64> names_;
    std::size_t size_ = 0;
};

[[nodiscard]] inline bool http_valid_mime_media_type(std::string_view value) noexcept {
    const auto parameters = http_find_unquoted_delimiter(value, 0, ';');
    const auto media_type = http_trim_ows(value.substr(0, parameters));
    const auto slash = media_type.find('/');
    if (slash == std::string_view::npos ||
        media_type.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    const auto type = media_type.substr(0, slash);
    const auto subtype = media_type.substr(slash + 1);
    if (type == "*" || subtype == "*" || !std::ranges::all_of(type, http_mime_token_char) ||
        !std::ranges::all_of(subtype, http_mime_token_char) || type.empty() || subtype.empty()) {
        return false;
    }
    http_mime_parameter_names parameter_names;
    return http_all_parameters(value, [&parameter_names](std::string_view parameter) noexcept {
        std::string_view name;
        std::string_view parameter_value;
        return http_parse_mime_parameter(parameter, name, parameter_value, false) &&
               parameter_names.record(name);
    });
}

}  // namespace ruvia::detail

// The multipart state machine: find the next delimiter, read one part's header
// block, then hand out that part's body in chunks -- driven entirely by what is
// currently buffered, so a caller may feed the body in any pieces.

namespace ruvia::detail {

enum class http_multipart_delimiter_kind : std::uint8_t {
    no_match,
    invalid,
    need_input,
    part,
    close,
};

// Delimiter scanning distinguishes absence, a malformed delimiter line, an
// input-boundary ambiguity, a regular part delimiter, and the terminal close
// delimiter. Candidate outcomes expose their offset; complete delimiter lines
// expose their line length.
struct http_multipart_delimiter_result final {
    http_multipart_delimiter_kind kind_{http_multipart_delimiter_kind::no_match};
    std::size_t offset_{0};
    std::size_t line_bytes_{0};

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_no_match() noexcept {
        return {.kind_ = http_multipart_delimiter_kind::no_match};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_invalid() noexcept {
        return {.kind_ = http_multipart_delimiter_kind::invalid};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_need_input(
        std::size_t offset = 0) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::need_input, .offset_ = offset};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_part(
        std::size_t offset, std::size_t line_bytes) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::part,
            .offset_ = offset,
            .line_bytes_ = line_bytes};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_close(
        std::size_t offset, std::size_t line_bytes) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::close,
            .offset_ = offset,
            .line_bytes_ = line_bytes};
    }

    [[nodiscard]] constexpr http_multipart_delimiter_result rebased(std::size_t base) const noexcept {
        if (kind_ == http_multipart_delimiter_kind::need_input ||
            kind_ == http_multipart_delimiter_kind::part ||
            kind_ == http_multipart_delimiter_kind::close) {
            return {.kind_ = kind_, .offset_ = base + offset_, .line_bytes_ = line_bytes_};
        }
        return *this;
    }
};

[[nodiscard]] inline bool http_multipart_marker_prefix_matches(
    std::string_view input, std::string_view boundary) noexcept {
    const auto expected_size = boundary.size() + 2;
    const auto compared = std::min(input.size(), expected_size);
    for (std::size_t index = 0; index < compared; ++index) {
        const char expected = index < 2 ? '-' : boundary[index - 2];
        if (input[index] != expected) {
            return false;
        }
    }
    return true;
}

// Matches one delimiter line beginning at its leading "--". RFC 2046
// transport-padding is accepted after a regular delimiter or after the closing
// "--". A closing delimiter ending exactly at the current buffer boundary is
// complete only when the I/O owner has signalled end-of-input.
[[nodiscard]] inline http_multipart_delimiter_result http_match_multipart_delimiter_line(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t& padding_cursor) noexcept {
    const auto value = boundary.value();
    const auto marker_size = value.size() + 2;
    if (!http_multipart_marker_prefix_matches(input, value)) {
        return http_multipart_delimiter_result::make_no_match();
    }
    if (input.size() < marker_size) {
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }

    std::size_t cursor_value = marker_size;
    bool close = false;
    if (cursor_value < input.size() && input[cursor_value] == '-') {
        if (cursor_value + 1 >= input.size()) {
            return input_finished ? http_multipart_delimiter_result::make_no_match()
                                  : http_multipart_delimiter_result::make_need_input();
        }
        if (input[cursor_value + 1] != '-') {
            return http_multipart_delimiter_result::make_invalid();
        }
        close = true;
        cursor_value += 2;
    }

    cursor_value = std::max(cursor_value, padding_cursor);
    // transport-padding is *LWSP-char (RFC 2046 section 5.1.1): SP / HTAB.
    while (cursor_value < input.size() && is_http_ows_char(input[cursor_value])) {
        ++cursor_value;
    }
    padding_cursor = cursor_value;
    if (cursor_value == input.size()) {
        if (close && input_finished) {
            return http_multipart_delimiter_result::make_close(0, cursor_value);
        }
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }
    if (input[cursor_value] != '\r') {
        return http_multipart_delimiter_result::make_invalid();
    }
    if (cursor_value + 1 >= input.size()) {
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }
    if (input[cursor_value + 1] != '\n') {
        return http_multipart_delimiter_result::make_invalid();
    }
    return close ? http_multipart_delimiter_result::make_close(0, cursor_value + 2)
                 : http_multipart_delimiter_result::make_part(0, cursor_value + 2);
}

// Cursors belong to one retained input prefix and must be reset after consume.
// Searches keep at most the three-byte delimiter prefix overlap; a candidate's
// transport padding resumes at its last inspected byte.
[[nodiscard]] inline http_multipart_delimiter_result http_find_multipart_delimiter_from_cursor(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    bool check_leading_dash, std::size_t rebase_crlf_offset,
    std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    if (check_leading_dash && search_cursor == 0 && !input.empty() && input.front() == '-') {
        auto match = http_match_multipart_delimiter_line(input, boundary, input_finished, padding_cursor);
        if (match.kind_ != http_multipart_delimiter_kind::no_match) {
            return match;
        }
        padding_cursor = 0;
    }
    for (auto prefix = input.find("\r\n--", search_cursor); prefix != std::string_view::npos;
        prefix = input.find("\r\n--", prefix + 1)) {
        auto match = http_match_multipart_delimiter_line(
            input.substr(prefix + 2), boundary, input_finished, padding_cursor);
        if (match.kind_ == http_multipart_delimiter_kind::no_match) {
            padding_cursor = 0;
            continue;
        }
        search_cursor = prefix;
        return match.rebased(prefix + rebase_crlf_offset);
    }
    search_cursor = input.size() > 3 ? input.size() - 3 : 0;
    return http_multipart_delimiter_result::make_no_match();
}

[[nodiscard]] inline http_multipart_delimiter_result http_find_initial_multipart_delimiter(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    auto match = http_find_multipart_delimiter_from_cursor(
        input, boundary, input_finished, true, 2, search_cursor, padding_cursor);
    if (match.kind_ != http_multipart_delimiter_kind::no_match) {
        return match;
    }
    // The CRLF belongs to the preamble, but the first marker byte may still
    // become a delimiter. Keep it out of the preamble quota until the next
    // byte resolves the candidate or EOF makes the prefix incomplete.
    if (!input_finished && input.ends_with("\r\n-")) {
        return http_multipart_delimiter_result::make_need_input(input.size() - 1);
    }
    return match;
}

// RFC 2046 section 5.1.1: body-part = MIME-part-headers [CRLF *OCTET], so a part
// may end right after its header block and the next delimiter's CRLF is the one
// that closed the headers. Body lines cannot start with the dash-boundary, so at
// the body start a delimiter line at offset 0 is a delimiter, not content.
[[nodiscard]] inline http_multipart_delimiter_result http_find_multipart_body_delimiter(
    std::string_view input, const multipart_boundary& boundary, bool input_finished, bool at_body_start,
    std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    return http_find_multipart_delimiter_from_cursor(
        input, boundary, input_finished, at_body_start, 0, search_cursor, padding_cursor);
}

[[nodiscard]] inline bool http_is_form_data_disposition(std::string_view disposition) noexcept {
    return http_ascii_equals_ignore_case(http_header_token_before_parameters(disposition), "form-data");
}

struct http_multipart_part_headers final {
    std::string_view name_;
    std::string_view filename_;
    std::string_view content_type_;
    bool filename_present_{false};

    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view filename() const noexcept {
        return filename_;
    }

    [[nodiscard]] constexpr bool has_filename() const noexcept {
        return filename_present_;
    }

    [[nodiscard]] constexpr std::string_view content_type() const noexcept {
        return content_type_;
    }
};

using http_multipart_part_header_parse_result =
    std::variant<http_multipart_part_headers, multipart_parse_error>;

[[nodiscard]] inline http_multipart_part_header_parse_result http_parse_multipart_part_headers(
    std::string_view headers) noexcept {
    std::optional<std::string_view> disposition;
    std::optional<std::string_view> content_type;
    auto remaining_headers = headers;
    while (!remaining_headers.empty()) {
        const auto line_end = remaining_headers.find("\r\n");
        const auto line = line_end == std::string_view::npos ? remaining_headers
                                                             : remaining_headers.substr(0, line_end);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || !http_valid_mime_field_name(line.substr(0, colon)) ||
            !is_valid_http_field_value_bytes(line.substr(colon + 1))) {
            return multipart_parse_error::invalid_part_headers;
        }
        const auto key = line.substr(0, colon);
        const auto value = http_trim_ows(line.substr(colon + 1));
        if (http_ascii_equals_ignore_case(key, "Content-Disposition")) {
            if (disposition) {
                return multipart_parse_error::invalid_content_disposition;
            }
            disposition = value;
        } else if (http_ascii_equals_ignore_case(key, "Content-Type")) {
            if (content_type || !http_valid_mime_media_type(value)) {
                return multipart_parse_error::invalid_part_headers;
            }
            content_type = value;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        remaining_headers.remove_prefix(line_end + 2);
    }

    if (!disposition || !http_is_form_data_disposition(*disposition)) {
        return multipart_parse_error::invalid_content_disposition;
    }

    // name and filename keep their wire spelling (token or validated
    // quoted-string); the parser decodes quoted-pairs only when it must own them.
    std::optional<std::string_view> name;
    std::optional<std::string_view> filename;
    http_mime_parameter_names parameter_names;
    const bool valid = http_all_parameters(*disposition, [&](std::string_view parameter) noexcept {
        std::string_view key;
        std::string_view value;
        if (!http_parse_mime_parameter(parameter, key, value, false) || !parameter_names.record(key)) {
            return false;
        }
        // RFC 7578 section 4.2 forbids senders to use filename*; recipients get no
        // reject rule, so it falls through with the other unknown parameters.
        if (http_ascii_equals_ignore_case(key, "name")) {
            name = value;
        } else if (http_ascii_equals_ignore_case(key, "filename")) {
            filename = value;
        }
        return true;
    });
    if (!valid) {
        return multipart_parse_error::invalid_content_disposition;
    }
    if (!name) {
        return multipart_parse_error::missing_field_name;
    }

    return http_multipart_part_headers{
        .name_ = *name,
        .filename_ = filename.value_or(std::string_view{}),
        .content_type_ = content_type.value_or(std::string_view{}),
        .filename_present_ = filename.has_value(),
    };
}

struct multipart_part_access final {
    using text_storage_type = multipart_part::text_storage_type;

    // Unmodified fields borrow the caller-owned body; unfolded or escaped fields
    // own string storage.
    [[nodiscard]] static multipart_part make_decoded(multipart_part::text_storage_type name,
        multipart_part::text_storage_type filename,
        multipart_part::text_storage_type content_type_value, std::string_view body,
        bool filename_present) noexcept {
        return multipart_part(std::move(name), std::move(filename),
            std::move(content_type_value), body, filename_present);
    }

    template <http_temporary_owning_char_string body_type>
    static multipart_part make_decoded(multipart_part::text_storage_type,
        multipart_part::text_storage_type, multipart_part::text_storage_type, body_type&&, bool) = delete;
};

struct multipart_stream_part_access final {
    [[nodiscard]] static constexpr multipart_stream_part make(std::string_view name,
        std::string_view filename, std::string_view content_type_value, std::string_view body,
        multipart_chunk_phase phase, bool filename_present) noexcept {
        return multipart_stream_part(name, filename, content_type_value, body, phase, filename_present);
    }

    template <http_temporary_owning_char_string name_type>
    static multipart_stream_part make(name_type&&, std::string_view, std::string_view, std::string_view,
        multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string filename_type>
    static multipart_stream_part make(std::string_view, filename_type&&, std::string_view,
        std::string_view, multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string content_type>
    static multipart_stream_part make(std::string_view, std::string_view, content_type&&,
        std::string_view, multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_stream_part make(std::string_view, std::string_view, std::string_view, body_type&&,
        multipart_chunk_phase, bool) = delete;
};

}  // namespace ruvia::detail

namespace ruvia {

namespace {

// Decodes a validated boundary parameter (token or quoted-string) into
// `decoded`; the boundary rule itself is checked by multipart_boundary.
[[nodiscard]] std::optional<std::string_view> decode_multipart_boundary_parameter(
    std::string_view parameter, std::array<char, 70>& decoded) noexcept {
    std::size_t size = 0;
    if (parameter.empty() || parameter.front() != '"') {
        if (parameter.size() > decoded.size()) {
            return std::nullopt;
        }
        return parameter;
    }
    http_quoted_string_cursor cursor(parameter);
    unsigned char byte = 0;
    while (cursor.next(byte)) {
        if (size >= decoded.size()) {
            return std::nullopt;
        }
        decoded[size++] = static_cast<char>(byte);
    }
    return std::string_view(decoded.data(), size);
}

}  // namespace

multipart_boundary_parse_result parse_multipart_boundary(std::string_view content_type_value) {
    if (!http_ascii_equals_ignore_case(
            detail::http_header_token_before_parameters(content_type_value), "multipart/form-data")) {
        return multipart_boundary_parse_result::make_not_applicable();
    }
    std::optional<multipart_boundary> boundary;
    std::array<char, 70> decoded{};
    detail::http_mime_parameter_names parameter_names;
    const bool valid = detail::http_all_parameters(content_type_value, [&](std::string_view parameter) {
        if (parameter.empty()) {
            return true;
        }
        std::string_view key;
        std::string_view value;
        if (!detail::http_parse_mime_parameter(parameter, key, value) || !parameter_names.record(key)) {
            return false;
        }
        if (http_ascii_equals_ignore_case(key, "boundary")) {
            const auto decoded_value = decode_multipart_boundary_parameter(value, decoded);
            boundary = decoded_value.has_value() ? multipart_boundary::try_create(*decoded_value)
                                                 : std::nullopt;
            if (!boundary) {
                return false;
            }
        }
        return true;
    });
    if (!valid || !boundary) {
        return multipart_boundary_parse_result::make_failure();
    }
    return multipart_boundary_parse_result(std::move(*boundary));
}

namespace {

constexpr std::size_t max_multipart_preamble_bytes = std::size_t{64} * 1024;
constexpr std::size_t max_multipart_header_bytes = std::size_t{64} * 1024;
constexpr std::size_t max_multipart_delimiter_line_bytes = std::size_t{64} * 1024;

// Appends a validated token or quoted-string parameter value with its
// quoted-pairs (RFC 9110 section 5.6.4) decoded.
void append_mime_parameter_value(std::string& out, std::string_view value) {
    if (value.empty() || value.front() != '"') {
        out.append(value);
        return;
    }
    http_quoted_string_cursor cursor(value);
    unsigned char byte = 0;
    while (cursor.next(byte)) {
        out.push_back(static_cast<char>(byte));
    }
}

[[nodiscard]] std::string_view multipart_text_view(
    const std::variant<std::string_view, std::string>& storage) noexcept {
    if (const auto* borrowed = std::get_if<std::string_view>(&storage)) {
        return *borrowed;
    }
    return std::get<std::string>(storage);
}

std::string& emplace_owned_multipart_text(
    std::variant<std::string_view, std::string>& storage) {
    if (auto* owned = std::get_if<std::string>(&storage)) {
        owned->clear();
        return *owned;
    }
    return storage.emplace<std::string>();
}

[[nodiscard]] http_protocol_error multipart_protocol_error(multipart_parse_error error) noexcept {
    switch (error) {
        case multipart_parse_error::incomplete_body:
            return http_protocol_error(http_status::bad_request, "multipart_incomplete_body",
                "incomplete multipart body");
        case multipart_parse_error::invalid_delimiter:
            return http_protocol_error(http_status::bad_request, "multipart_invalid_delimiter",
                "invalid multipart delimiter");
        case multipart_parse_error::preamble_too_large:
            return http_protocol_error(http_status::content_too_large,
                "multipart_preamble_too_large", "multipart preamble exceeds limit");
        case multipart_parse_error::part_headers_too_large:
            return http_protocol_error(http_status::content_too_large,
                "multipart_part_headers_too_large", "multipart part headers exceed limit");
        case multipart_parse_error::invalid_part_headers:
            return http_protocol_error(http_status::bad_request, "multipart_invalid_part_headers",
                "invalid multipart part headers");
        case multipart_parse_error::invalid_content_disposition:
            return http_protocol_error(http_status::bad_request,
                "multipart_invalid_content_disposition", "invalid multipart content disposition");
        case multipart_parse_error::missing_field_name:
            return http_protocol_error(http_status::bad_request, "multipart_missing_field_name",
                "invalid multipart field name");
        case multipart_parse_error::delimiter_line_too_large:
            return http_protocol_error(http_status::content_too_large,
                "multipart_delimiter_line_too_large", "multipart delimiter line exceeds limit");
        case multipart_parse_error::too_many_parts:
            return http_protocol_error(http_status::content_too_large, "multipart_too_many_parts",
                "multipart part count exceeds limit");
        case multipart_parse_error::metadata_too_large:
            return http_protocol_error(http_status::content_too_large,
                "multipart_metadata_too_large", "multipart metadata exceeds limit");
    }
    return http_protocol_error(
        http_status::bad_request, "multipart_invalid_body", "invalid multipart body");
}

}  // namespace

http_protocol_error multipart_poll_failure::protocol_error() const noexcept {
    return multipart_protocol_error(error_);
}

multipart_parser::multipart_parser(multipart_parse_options options)
    : boundary_(std::move(options.boundary_)),
      input_(std::in_place_type<http_io_buffer>),
      remaining_parts_(options.max_parts_),
      remaining_metadata_bytes_(options.max_metadata_bytes_) {}

multipart_parser::multipart_parser(
    std::string_view complete_body, multipart_parse_options options, complete_input_tag_type)
    : boundary_(std::move(options.boundary_)),
      input_(complete_body),
      remaining_parts_(options.max_parts_),
      remaining_metadata_bytes_(options.max_metadata_bytes_),
      input_eof_(true) {}

std::vector<multipart_part> parse_multipart_body(
    std::string_view body, multipart_parse_options options) {
    multipart_parser parser(body, std::move(options), multipart_parser::complete_input_tag_type{});
    std::vector<multipart_part> parts;
    for (;;) {
        auto result_value = parser.poll();
        if (const auto* part = result_value.part()) {
            parts.push_back(detail::multipart_part_access::make_decoded(
                std::exchange(parser.current_name_, std::string_view{}),
                std::exchange(parser.current_filename_, std::string_view{}),
                std::exchange(parser.current_content_type_, std::string_view{}), part->body(),
                part->has_filename()));
            continue;
        }
        if (result_value.done() != nullptr) {
            return parts;
        }
        if (const auto* failure = result_value.failure()) {
            throw failure->protocol_error();
        }
        throw multipart_protocol_error(multipart_parse_error::incomplete_body);
    }
}

std::string_view multipart_parser::buffer_view() const noexcept {
    if (const auto* borrowed = std::get_if<std::string_view>(&input_)) {
        return *borrowed;
    }
    return std::get<http_io_buffer>(input_).view();
}

void multipart_parser::consume(std::size_t bytes_value) noexcept {
    if (auto* borrowed = std::get_if<std::string_view>(&input_)) {
        borrowed->remove_prefix(std::min(bytes_value, borrowed->size()));
    } else {
        std::get<http_io_buffer>(input_).consume(bytes_value);
    }
    header_scan_offset_ = header_scan_offset_ >= bytes_value ? header_scan_offset_ - bytes_value : 0;
    delimiter_scan_offset_ =
        delimiter_scan_offset_ >= bytes_value ? delimiter_scan_offset_ - bytes_value : 0;
}

void multipart_parser::reset_scan_cursors() noexcept {
    header_scan_offset_ = 0;
    delimiter_scan_offset_ = 0;
    delimiter_padding_offset_ = 0;
}

void multipart_parser::compact_pending() {
    if (pending_erase_bytes_ == 0) {
        return;
    }
    consume(pending_erase_bytes_);
    pending_erase_bytes_ = 0;
}

void multipart_parser::feed(std::string_view chunk) {
    auto* owned = std::get_if<http_io_buffer>(&input_);
    if (owned == nullptr || input_eof_ || (state_.index() != 0) ||
        std::get<0>(state_) == progress_state_type::done) {
        throw std::logic_error("multipart parser cannot accept input in a terminal state");
    }
    owned->append(chunk);
}

void multipart_parser::finish_input() noexcept {
    input_eof_ = true;
}

multipart_poll_result multipart_parser::fail(multipart_parse_error error) noexcept {
    auto result_value = multipart_poll_result::make_failure(error);
    state_ = error;
    return result_value;
}

multipart_poll_result multipart_parser::poll() {
    if ((state_.index() != 0)) {
        return multipart_poll_result::make_failure(std::get<1>(state_));
    }
    for (;;) {
        compact_pending();
        switch (std::get<0>(state_)) {
            case progress_state_type::boundary: {
                const auto step = process_boundary();
                if ((step.index() != 0)) {
                    return fail(std::get<1>(step));
                }
                const auto progress_value = std::get<0>(step);
                if (progress_value == step_progress_type::need_input) {
                    if (input_eof_) {
                        return fail(multipart_parse_error::incomplete_body);
                    }
                    return multipart_poll_result::make_need_input();
                }
                if (progress_value == step_progress_type::done) {
                    return multipart_poll_result::make_done();
                }
                break;
            }
            case progress_state_type::headers: {
                const auto step = process_headers();
                if ((step.index() != 0)) {
                    return fail(std::get<1>(step));
                }
                const auto progress_value = std::get<0>(step);
                if (progress_value == step_progress_type::need_input) {
                    if (input_eof_) {
                        return fail(multipart_parse_error::incomplete_body);
                    }
                    return multipart_poll_result::make_need_input();
                }
                break;
            }
            case progress_state_type::body: {
                auto result_value = read_body_chunk();
                if (result_value.need_input() != nullptr && input_eof_) {
                    return fail(multipart_parse_error::incomplete_body);
                }
                return result_value;
            }
            case progress_state_type::done:
                return multipart_poll_result::make_done();
        }
    }
}

multipart_parser::step_result_type multipart_parser::process_boundary() {
    // RFC 2046 section 5.1.1: the first boundary may be preceded by a preamble that
    // is ignored. Skip it once, reusing the buffered parser's boundary finder so
    // the streaming and buffered paths accept exactly the same bodies.
    if (first_boundary_) {
        const auto delimiter =
            detail::http_find_initial_multipart_delimiter(buffer_view(), boundary_, input_eof_,
                delimiter_scan_offset_, delimiter_padding_offset_);
        if (delimiter.kind_ == detail::http_multipart_delimiter_kind::invalid) {
            return multipart_parse_error::invalid_delimiter;
        }
        if (delimiter.kind_ == detail::http_multipart_delimiter_kind::no_match) {
            if (buffer_view().size() > max_multipart_preamble_bytes) {
                return multipart_parse_error::preamble_too_large;
            }
            return step_progress_type::need_input;
        }
        if (delimiter.kind_ == detail::http_multipart_delimiter_kind::need_input) {
            const auto buffer_bytes = buffer_view().size();
            if (delimiter.offset_ > max_multipart_preamble_bytes) {
                return multipart_parse_error::preamble_too_large;
            }
            if (buffer_bytes - delimiter.offset_ > max_multipart_delimiter_line_bytes) {
                return multipart_parse_error::delimiter_line_too_large;
            }
            return step_progress_type::need_input;
        }
        if (delimiter.offset_ > max_multipart_preamble_bytes) {
            return multipart_parse_error::preamble_too_large;
        }
        if (delimiter.line_bytes_ > max_multipart_delimiter_line_bytes) {
            return multipart_parse_error::delimiter_line_too_large;
        }
        consume(delimiter.offset_ + delimiter.line_bytes_);
        reset_scan_cursors();
        first_boundary_ = false;
        if (delimiter.kind_ == detail::http_multipart_delimiter_kind::part) {
            state_ = progress_state_type::headers;
            return step_progress_type::continue_value;
        }
        state_ = progress_state_type::done;
        return step_progress_type::done;
    }

    if (buffer_view().starts_with("\r\n")) {
        consume(2);
        reset_scan_cursors();
    }

    const auto delimiter =
        detail::http_match_multipart_delimiter_line(buffer_view(), boundary_, input_eof_,
            delimiter_padding_offset_);
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::need_input) {
        if (buffer_view().size() > max_multipart_delimiter_line_bytes) {
            return multipart_parse_error::delimiter_line_too_large;
        }
        return step_progress_type::need_input;
    }
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::part ||
        delimiter.kind_ == detail::http_multipart_delimiter_kind::close) {
        if (delimiter.line_bytes_ > max_multipart_delimiter_line_bytes) {
            return multipart_parse_error::delimiter_line_too_large;
        }
        consume(delimiter.line_bytes_);
        reset_scan_cursors();
        if (delimiter.kind_ == detail::http_multipart_delimiter_kind::part) {
            state_ = progress_state_type::headers;
            return step_progress_type::continue_value;
        }
        state_ = progress_state_type::done;
        return step_progress_type::done;
    }
    return multipart_parse_error::invalid_delimiter;
}

multipart_parser::step_result_type multipart_parser::process_headers() {
    // Cap on a single part's header block, mirroring the 64KB request-header limit.
    const auto buffer = buffer_view();
    const auto headers_end = buffer.find("\r\n\r\n", header_scan_offset_);
    if (headers_end == std::string_view::npos) {
        header_scan_offset_ = buffer.size() > 3 ? buffer.size() - 3 : 0;
        if (buffer.size() > max_multipart_header_bytes) {
            return multipart_parse_error::part_headers_too_large;
        }
        return step_progress_type::need_input;
    }
    // Include the terminating CRLF CRLF in the same byte cap. Checking only
    // the incomplete path allowed an oversized but already-terminated block
    // delivered in one feed() to bypass the limit entirely.
    if (headers_end > max_multipart_header_bytes - 4) {
        return multipart_parse_error::part_headers_too_large;
    }

    if (remaining_parts_ == 0) {
        return multipart_parse_error::too_many_parts;
    }
    if (headers_end + 4 > remaining_metadata_bytes_) {
        return multipart_parse_error::metadata_too_large;
    }
    auto headers = buffer.substr(0, headers_end);
    const auto first_fold = detail::find_field_line_fold(headers);
    const bool unfolded = first_fold != std::string_view::npos;
    if (unfolded) {
        detail::copy_unfolded_field_lines(
            headers, first_fold, detail::field_line_folding_policy::mime, current_headers_);
        headers = current_headers_;
    }
    const auto parsed_headers = detail::http_parse_multipart_part_headers(headers);
    if (const auto* error = std::get_if<multipart_parse_error>(&parsed_headers)) {
        return *error;
    }
    const auto* part_headers = std::get_if<detail::http_multipart_part_headers>(&parsed_headers);

    const bool borrowed_headers = std::holds_alternative<std::string_view>(input_) && !unfolded;
    if (borrowed_headers && part_headers->name().find('\\') == std::string_view::npos) {
        current_name_ = http_trim_quoted_field_value(part_headers->name());
    } else {
        append_mime_parameter_value(
            emplace_owned_multipart_text(current_name_), part_headers->name());
    }
    has_current_filename_ = part_headers->has_filename();
    if (has_current_filename_ && !http_trim_quoted_field_value(part_headers->filename()).empty()) {
        if (borrowed_headers && part_headers->filename().find('\\') == std::string_view::npos) {
            current_filename_ = http_trim_quoted_field_value(part_headers->filename());
        } else {
            append_mime_parameter_value(
                emplace_owned_multipart_text(current_filename_), part_headers->filename());
        }
    } else {
        current_filename_ = std::string_view{};
    }
    if (part_headers->content_type().empty()) {
        current_content_type_ = std::string_view{};
    } else if (borrowed_headers) {
        current_content_type_ = part_headers->content_type();
    } else {
        emplace_owned_multipart_text(current_content_type_)
            .assign(part_headers->content_type().data(), part_headers->content_type().size());
    }
    --remaining_parts_;
    remaining_metadata_bytes_ -= headers_end + 4;
    consume(headers_end + 4);
    reset_scan_cursors();
    next_chunk_is_first_ = true;
    state_ = progress_state_type::body;
    return step_progress_type::continue_value;
}

multipart_stream_part multipart_parser::make_part(std::string_view body, bool part_end) {
    const auto phase =
        next_chunk_is_first_ ? (part_end ? multipart_chunk_phase::complete : multipart_chunk_phase::first)
                             : (part_end ? multipart_chunk_phase::last : multipart_chunk_phase::middle);
    auto part = detail::multipart_stream_part_access::make(multipart_text_view(current_name_),
        multipart_text_view(current_filename_), multipart_text_view(current_content_type_), body,
        phase, has_current_filename_);
    next_chunk_is_first_ = false;
    return part;
}

multipart_poll_result multipart_parser::read_body_chunk() {
    const auto buffer = buffer_view();
    const auto delimiter =
        detail::http_find_multipart_body_delimiter(buffer, boundary_, input_eof_, next_chunk_is_first_,
            delimiter_scan_offset_, delimiter_padding_offset_);
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::invalid) {
        return fail(multipart_parse_error::invalid_delimiter);
    }
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::part ||
        delimiter.kind_ == detail::http_multipart_delimiter_kind::close) {
        if (delimiter.line_bytes_ > max_multipart_delimiter_line_bytes) {
            return fail(multipart_parse_error::delimiter_line_too_large);
        }
        const auto crlf_bytes =
            buffer.substr(delimiter.offset_).starts_with("\r\n") ? std::size_t{2} : std::size_t{0};
        auto part = make_part(buffer.substr(0, delimiter.offset_), true);
        pending_erase_bytes_ = delimiter.offset_ + crlf_bytes + delimiter.line_bytes_;
        reset_scan_cursors();
        state_ = delimiter.kind_ == detail::http_multipart_delimiter_kind::part
                     ? progress_state_type::headers
                     : progress_state_type::done;
        return multipart_poll_result::make_part(part);
    }
    if (delimiter.kind_ == detail::http_multipart_delimiter_kind::need_input) {
        if (buffer.size() - delimiter.offset_ > max_multipart_delimiter_line_bytes + 2) {
            return fail(multipart_parse_error::delimiter_line_too_large);
        }
        if (delimiter.offset_ > 0) {
            auto part = make_part(buffer.substr(0, delimiter.offset_), false);
            pending_erase_bytes_ = delimiter.offset_;
            return multipart_poll_result::make_part(part);
        }
        return multipart_poll_result::make_need_input();
    }

    const auto keep_tail = boundary_.value().size() + 8;
    if (!input_eof_ && buffer.size() > keep_tail) {
        const auto bytes_value = buffer.size() - keep_tail;
        auto part = make_part(buffer.substr(0, bytes_value), false);
        pending_erase_bytes_ = bytes_value;
        return multipart_poll_result::make_part(part);
    }

    return multipart_poll_result::make_need_input();
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Keeps every offset and size representable as a pointer difference.
constexpr std::size_t max_io_buffer_bytes = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());

}  // namespace

http_io_buffer::http_io_buffer(http_io_buffer&& other) noexcept
    : storage_(std::exchange(other.storage_, nullptr)),
      capacity_(std::exchange(other.capacity_, 0)),
      begin_(std::exchange(other.begin_, 0)),
      end_(std::exchange(other.end_, 0)) {}

http_io_buffer::~http_io_buffer() {
    release();
}

std::span<char> http_io_buffer::prepare(std::size_t bytes) & {
    if (capacity_ - end_ < bytes) {
        make_room(bytes);
    }
    return std::span<char>(storage_ + end_, bytes);
}

std::span<char> http_io_buffer::prepare_spare() & noexcept {
    if (end_ == capacity_ && begin_ >= size()) {
        move_window_to_front();
    }
    return std::span<char>(storage_ + end_, capacity_ - end_);
}

void http_io_buffer::commit(std::size_t bytes) {
    if (bytes > capacity_ - end_) {
        throw std::length_error("http_io_buffer commit exceeds the prepared space");
    }
    end_ += bytes;
}

void http_io_buffer::append(std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    if (capacity_ - end_ >= bytes.size()) {
        // The source may be readable bytes of this buffer, which never
        // overlap the writable space but may share its storage.
        std::memmove(storage_ + end_, bytes.data(), bytes.size());
        end_ += bytes.size();
        return;
    }
    const auto capacity = room_capacity(bytes.size());
    if (capacity > capacity_ || detail::http_text_overlaps(bytes, std::string_view(storage_, capacity_))) {
        // Publish the appended bytes together with the replacement storage.
        reallocate(capacity, bytes);
        return;
    }
    move_window_to_front();
    std::memcpy(storage_ + end_, bytes.data(), bytes.size());
    end_ += bytes.size();
}

void http_io_buffer::consume(std::size_t bytes) noexcept {
    begin_ += std::min(bytes, size());
    if (begin_ == end_) {
        clear();
    }
}

void http_io_buffer::reserve(std::size_t bytes) {
    if (bytes > max_io_buffer_bytes) {
        throw std::length_error("http_io_buffer size exceeds the addressable range");
    }
    if (bytes > capacity_) {
        reallocate(bytes);
    }
}

void http_io_buffer::shrink_to(std::size_t bytes) {
    const auto target = std::max(size(), bytes);
    if (capacity_ > target) {
        reallocate(target);
    }
}

void http_io_buffer::clear_retaining_small(std::size_t retained_bytes) noexcept {
    clear();
    if (capacity_ > retained_bytes) {
        release();
    }
}

std::size_t http_io_buffer::room_capacity(std::size_t bytes) const {
    if (bytes > max_io_buffer_bytes - size()) {
        throw std::length_error("http_io_buffer size exceeds the addressable range");
    }
    // Charge compaction to the consumed prefix: moving the live bytes must
    // recover at least as many bytes. Otherwise grow once for future writes.
    if (capacity_ - size() >= bytes && begin_ >= size()) {
        return capacity_;
    }
    const auto doubled = capacity_ > max_io_buffer_bytes / 2 ? max_io_buffer_bytes : capacity_ * 2;
    return std::max(size() + bytes, doubled);
}

void http_io_buffer::make_room(std::size_t bytes) {
    const auto capacity = room_capacity(bytes);
    if (capacity > capacity_) {
        reallocate(capacity);
        return;
    }
    move_window_to_front();
}

void http_io_buffer::move_window_to_front() noexcept {
    if (begin_ == 0) {
        return;
    }
    const auto live = size();
    std::memmove(storage_, storage_ + begin_, live);
    begin_ = 0;
    end_ = live;
}

void http_io_buffer::reallocate(std::size_t capacity, std::string_view tail) {
    const auto live = size();
    char* storage = capacity == 0 ? nullptr : std::allocator<char>{}.allocate(capacity);
    if (live != 0) {
        std::memcpy(storage, storage_ + begin_, live);
    }
    if (!tail.empty()) {
        std::memcpy(storage + live, tail.data(), tail.size());
    }
    // Publish the complete window before old storage can reenter its owner.
    http_io_buffer retired(std::move(*this));
    storage_ = storage;
    capacity_ = capacity;
    begin_ = 0;
    end_ = live + tail.size();
}

void http_io_buffer::release() noexcept {
    auto* const storage = std::exchange(storage_, nullptr);
    const auto capacity = std::exchange(capacity_, 0);
    begin_ = 0;
    end_ = 0;
    if (storage != nullptr) {
        std::allocator<char>{}.deallocate(storage, capacity);
    }
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

constexpr std::size_t allocation_alignment = alignof(std::max_align_t);
// The prefix keeps the payload max-aligned while recording the block size.
constexpr std::size_t allocation_prefix_bytes =
    ((sizeof(std::size_t) + allocation_alignment - 1) / allocation_alignment) * allocation_alignment;

[[nodiscard]] std::byte* allocation_base(void* pointer) noexcept {
    return static_cast<std::byte*>(pointer) - allocation_prefix_bytes;
}

[[nodiscard]] std::size_t allocation_payload_size(void* pointer) noexcept {
    std::size_t total = 0;
    std::memcpy(&total, allocation_base(pointer), sizeof(total));
    return total - allocation_prefix_bytes;
}

}  // namespace

http_c_library_memory::http_c_library_memory(
    std::size_t native_budget, bool throw_on_budget_exceeded) noexcept
    : native_budget_(native_budget),
      throw_on_budget_exceeded_(throw_on_budget_exceeded) {}

void http_c_library_memory::rethrow_if_failed() const {
    if (auto failure = std::exchange(failure_, nullptr)) {
        std::rethrow_exception(failure);
    }
}

void http_c_library_memory::rethrow_allocation_failure() const {
    rethrow_if_failed();
    throw std::bad_alloc();
}

void* http_c_library_memory::allocate(std::size_t size, void* context) noexcept {
    if (context == nullptr) {
        return nullptr;
    }
    auto& self = *static_cast<http_c_library_memory*>(context);
    if (size > self.native_budget_ - self.live_bytes_) {
        self.budget_exceeded_ = true;
        if (self.throw_on_budget_exceeded_ && !self.failure_) {
            self.failure_ = std::make_exception_ptr(std::bad_alloc{});
        }
        return nullptr;
    }
    if (size > (std::numeric_limits<std::size_t>::max)() - allocation_prefix_bytes) {
        return nullptr;
    }
    const auto total = size + allocation_prefix_bytes;
    try {
        auto* base = static_cast<std::byte*>(::operator new(total, std::align_val_t{allocation_alignment}));
        std::memcpy(base, &total, sizeof(total));
        self.live_bytes_ += size;
        return base + allocation_prefix_bytes;
    } catch (...) {
        if (!self.failure_) {
            self.failure_ = std::current_exception();
        }
        return nullptr;
    }
}

void http_c_library_memory::deallocate(void* pointer, void* context) noexcept {
    if (pointer == nullptr) {
        return;
    }
    auto& self = *static_cast<http_c_library_memory*>(context);
    std::size_t total = 0;
    std::memcpy(&total, allocation_base(pointer), sizeof(total));
    ::operator delete(allocation_base(pointer), total, std::align_val_t{allocation_alignment});
    self.live_bytes_ -= total - allocation_prefix_bytes;
}

void* http_c_library_memory::allocate_zeroed(std::size_t count, std::size_t size, void* context) noexcept {
    if (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size) {
        return nullptr;
    }
    auto* pointer = allocate(count * size, context);
    if (pointer != nullptr) {
        std::memset(pointer, 0, count * size);
    }
    return pointer;
}

void* http_c_library_memory::reallocate(void* pointer, std::size_t size, void* context) noexcept {
    if (pointer == nullptr) {
        return allocate(size, context);
    }
    if (size == 0) {
        deallocate(pointer, context);
        return nullptr;
    }
    auto* replacement = allocate(size, context);
    if (replacement == nullptr) {
        // C realloc semantics: the original block stays valid on failure.
        return nullptr;
    }
    std::memcpy(replacement, pointer, (std::min)(size, allocation_payload_size(pointer)));
    deallocate(pointer, context);
    return replacement;
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

// Splits `data` at CR, LF or CRLF and passes each line to `visitor`; the
// sizing pass and the encoder share this one line rule.
template <typename visitor_type>
void visit_sse_data_lines(std::string_view data, visitor_type&& visitor) {
    while (true) {
        const auto next_value = data.find_first_of("\r\n");
        visitor(next_value == std::string_view::npos ? data : data.substr(0, next_value));
        if (next_value == std::string_view::npos) {
            return;
        }
        auto advance = next_value + 1U;
        if (data[next_value] == '\r' && next_value + 1U < data.size() && data[next_value + 1U] == '\n') {
            advance = next_value + 2U;
        }
        data.remove_prefix(advance);
    }
}

constexpr std::string_view sse_utf8_bom = "\xEF\xBB\xBF";
constexpr std::string_view sse_replacement_character = "\xEF\xBF\xBD";
constexpr std::string_view sse_default_event_type = "message";

// The event stream is decoded with WHATWG UTF-8 decode, which replaces each
// maximal subpart of an ill-formed sequence with one U+FFFD. The decoder never
// consumes an ASCII byte, so repairing each field value after the line split
// matches decoding the stream first.
void replace_ill_formed_utf8(std::string& value) {
    const std::string_view bytes(value);
    std::size_t index = 0;
    while (index < bytes.size()) {
        if (static_cast<unsigned char>(bytes[index]) < 0x80) {
            ++index;
            continue;
        }
        const auto sequence = scan_utf8_sequence(bytes, index);
        if (!sequence.well_formed_) {
            break;
        }
        index += sequence.length_;
    }
    if (index == bytes.size()) {
        return;
    }
    std::string repaired;
    repaired.reserve(bytes.size());
    repaired.append(bytes.substr(0, index));
    while (index < bytes.size()) {
        if (static_cast<unsigned char>(bytes[index]) < 0x80) {
            repaired.push_back(bytes[index]);
            ++index;
            continue;
        }
        const auto sequence = scan_utf8_sequence(bytes, index);
        repaired.append(sequence.well_formed_ ? bytes.substr(index, sequence.length_) : sse_replacement_character);
        index += sequence.length_;
    }
    value.swap(repaired);
}

}  // namespace

sse_parser::sse_parser(sse_parser_options options)
    : event_limit_(options.event_limit_) {}

sse_parse_result sse_parser::parse(std::string_view input) & {
    if (state_ != sse_parser_state::active) {
        throw std::logic_error("SSE parser is not active");
    }
    release_event_buffers();
    std::size_t position = 0;
    if (!bom_settled_) {
        while (position < input.size() && bom_matched_ < sse_utf8_bom.size() &&
               input[position] == sse_utf8_bom[bom_matched_]) {
            ++bom_matched_;
            ++position;
        }
        if (bom_matched_ < sse_utf8_bom.size() && position == input.size()) {
            return {.consumed_bytes_ = position};
        }
        bom_settled_ = true;
        if (bom_matched_ < sse_utf8_bom.size()) {
            // Not a BOM: the bytes matched so far begin the first field name.
            append_name(sse_utf8_bom.substr(0, bom_matched_));
        }
    }
    while (position < input.size()) {
        if (std::exchange(skip_line_feed_, false) && input[position] == '\n') {
            ++position;
            continue;
        }
        const auto rest = input.substr(position);
        if (phase_ == line_phase::value_start) {
            phase_ = line_phase::value;
            if (rest.front() == ' ') {
                ++position;
            }
            continue;
        }
        if (phase_ == line_phase::name) {
            const auto stop = rest.find_first_of(":\r\n");
            append_name(rest.substr(0, stop));
            if (stop == std::string_view::npos) {
                return {.consumed_bytes_ = input.size()};
            }
            position += stop + 1;
            const bool unnamed = name_size_ == 0 && !name_overflow_;
            if (!unnamed) {
                classify_field();
            }
            if (rest[stop] == ':') {
                phase_ = unnamed ? line_phase::comment : line_phase::value_start;
                continue;
            }
            skip_line_feed_ = rest[stop] == '\r';
            if (unnamed) {
                if (dispatch()) {
                    return {.consumed_bytes_ = position,
                        .event_ = sse_event(event_type_.empty() ? sse_default_event_type : std::string_view(event_type_),
                            data_, last_event_id_)};
                }
                continue;
            }
            // A line without a colon names a field with an empty value.
            if (!end_line()) {
                fail();
                return {.consumed_bytes_ = position};
            }
            continue;
        }
        const auto stop = rest.find_first_of("\r\n");
        const auto bytes = rest.substr(0, stop);
        if (phase_ == line_phase::value && !append_value(bytes)) {
            fail();
            return {.consumed_bytes_ = position};
        }
        if (stop == std::string_view::npos) {
            return {.consumed_bytes_ = input.size()};
        }
        position += stop + 1;
        skip_line_feed_ = rest[stop] == '\r';
        if (phase_ == line_phase::comment) {
            reset_line();
        } else if (!end_line()) {
            fail();
            return {.consumed_bytes_ = position};
        }
    }
    return {.consumed_bytes_ = position};
}

void sse_parser::finish() noexcept {
    if (state_ != sse_parser_state::active) {
        return;
    }
    release_event_buffers();
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(value_);
    state_ = sse_parser_state::finished;
}

void sse_parser::reset() noexcept {
    event_published_ = false;
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(value_);
    http_clear_string_retaining_small(last_event_id_buffer_);
    http_clear_string_retaining_small(last_event_id_);
    reconnection_time_.reset();
    skip_line_feed_ = false;
    bom_matched_ = 0;
    bom_settled_ = false;
    state_ = sse_parser_state::active;
}

std::size_t sse_parser::buffered_bytes() const noexcept {
    return data_.size() + event_type_.size() + value_.size();
}

void sse_parser::append_name(std::string_view bytes) noexcept {
    if (name_overflow_ || bytes.size() > name_.size() - name_size_) {
        name_overflow_ = name_overflow_ || !bytes.empty();
        return;
    }
    bytes.copy(name_.data() + name_size_, bytes.size());
    name_size_ = static_cast<std::uint8_t>(name_size_ + bytes.size());
}

void sse_parser::classify_field() noexcept {
    const std::string_view name(name_.data(), name_overflow_ ? 0 : name_size_);
    field_ = name == "data"    ? field_kind::data
             : name == "event" ? field_kind::event
             : name == "id"    ? field_kind::id
             : name == "retry" ? field_kind::retry
                               : field_kind::ignored;
}

bool sse_parser::append_value(std::string_view bytes) {
    if (field_ == field_kind::retry) {
        // Only ASCII digits make a valid reconnection time; a value beyond
        // the duration range saturates.
        constexpr auto max_retry = static_cast<std::uint64_t>((std::numeric_limits<std::chrono::milliseconds::rep>::max)());
        for (const char byte : bytes) {
            if (byte < '0' || byte > '9') {
                retry_valid_ = false;
                break;
            }
            const auto digit = static_cast<std::uint64_t>(byte - '0');
            retry_digits_ = true;
            retry_value_ = retry_value_ > (max_retry - digit) / 10 ? max_retry : retry_value_ * 10 + digit;
        }
        return true;
    }
    if (field_ == field_kind::ignored || bytes.empty()) {
        return true;
    }
    if (event_limit_.addition_exceeds(buffered_bytes(), bytes.size())) {
        return false;
    }
    (field_ == field_kind::data ? data_ : value_).append(bytes);
    return true;
}

bool sse_parser::end_line() {
    switch (field_) {
        case field_kind::data:
            if (event_limit_.addition_exceeds(buffered_bytes(), 1)) {
                return false;
            }
            data_.push_back('\n');
            break;
        case field_kind::event:
            replace_ill_formed_utf8(value_);
            event_type_.swap(value_);
            break;
        case field_kind::id:
            // An id containing NUL is ignored.
            if (value_.find('\0') == std::string::npos) {
                replace_ill_formed_utf8(value_);
                last_event_id_buffer_.swap(value_);
            }
            break;
        case field_kind::retry:
            if (retry_digits_ && retry_valid_) {
                reconnection_time_ = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(retry_value_));
            }
            break;
        case field_kind::ignored:
            break;
    }
    reset_line();
    return true;
}

bool sse_parser::dispatch() {
    last_event_id_ = last_event_id_buffer_;
    if (data_.empty()) {
        http_clear_string_retaining_small(event_type_);
        return false;
    }
    data_.pop_back();
    replace_ill_formed_utf8(data_);
    event_published_ = true;
    return true;
}

void sse_parser::reset_line() noexcept {
    phase_ = line_phase::name;
    field_ = field_kind::ignored;
    name_size_ = 0;
    name_overflow_ = false;
    value_.clear();
    retry_value_ = 0;
    retry_digits_ = false;
    retry_valid_ = true;
}

void sse_parser::release_event_buffers() noexcept {
    if (std::exchange(event_published_, false)) {
        http_clear_string_retaining_small(data_);
        http_clear_string_retaining_small(event_type_);
    }
}

void sse_parser::fail() noexcept {
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(value_);
    state_ = sse_parser_state::failed;
}

std::string format_sse_message(const sse_message& message) {
    if (message.event_.view().find_first_of("\r\n") != std::string_view::npos ||
        (message.id_.has_value() &&
            message.id_->view().find_first_of("\r\n") != std::string_view::npos)) {
        throw std::invalid_argument("SSE event and id must not contain CR or LF");
    }
    if (message.id_.has_value() && (message.id_->view().find('\0') != std::string_view::npos)) {
        throw std::invalid_argument("SSE id must not contain a NUL character");
    }
    if (message.retry_.has_value() && message.retry_->count() < 0) {
        throw std::invalid_argument("SSE retry delay must not be negative");
    }
    // An event stream is UTF-8 (WHATWG HTML, "Parsing an event stream").
    if (!is_valid_utf8(message.event_.view()) ||
        (message.id_.has_value() && !is_valid_utf8(message.id_->view())) ||
        (message.data_.has_value() && !is_valid_utf8(message.data_->view()))) {
        throw std::invalid_argument("SSE fields must be valid UTF-8");
    }

    std::string frame;
    std::array<char, http_max_unsigned_decimal_size> retry_buffer{};
    std::string_view retry_digits{};
    std::size_t total_size = 1U;
    const auto add_size = [&frame, &total_size](std::size_t bytes) {
        if (bytes > frame.max_size() - total_size) {
            throw std::length_error("SSE frame exceeds string capacity");
        }
        total_size += bytes;
    };
    if (!message.event_.empty()) {
        add_size(7U);
        add_size(message.event_.size());
        add_size(1U);
    }
    if (message.id_.has_value()) {
        add_size(3U);
        if (!message.id_->empty()) {
            add_size(1U);
            add_size(message.id_->size());
        }
        add_size(1U);
    }
    if (message.retry_.has_value()) {
        retry_digits = http_format_unsigned(
            retry_buffer, static_cast<std::uint64_t>(message.retry_->count()));
        add_size(7U);
        add_size(retry_digits.size());
        add_size(1U);
    }
    if (message.data_.has_value()) {
        visit_sse_data_lines(message.data_->view(), [&add_size](std::string_view line) {
            add_size(6U);
            add_size(line.size());
            add_size(1U);
        });
    }

    frame.reserve(total_size);
    if (!message.event_.empty()) {
        frame.append("event: ");
        frame.append(message.event_.data(), message.event_.size());
        frame.push_back('\n');
    }
    if (message.id_.has_value()) {
        frame.append("id:");
        if (!message.id_->empty()) {
            frame.push_back(' ');
            frame.append(message.id_->data(), message.id_->size());
        }
        frame.push_back('\n');
    }
    if (message.retry_.has_value()) {
        frame.append("retry: ");
        frame.append(retry_digits);
        frame.push_back('\n');
    }
    if (message.data_.has_value()) {
        visit_sse_data_lines(message.data_->view(), [&frame](std::string_view line) {
            frame.append("data: ");
            frame.append(line.data(), line.size());
            frame.push_back('\n');
        });
    }
    frame.push_back('\n');
    return frame;
}

}  // namespace ruvia

// What a caller-supplied request header section must satisfy before the writer
// will encode it, and the facts the encoder then needs from it: its field
// section budget, which singleton fields it already carries, and the
// connection and upgrade options it expresses.

namespace ruvia::detail {

struct http1_client_request_field_facts final {
    // The caller's fields, charged by the shared field-section rule.
    http_field_section_budget section_;
    detail::request_field_state fields_{detail::request_field_framing::http1};
    detail::http_connection_options connection_options_;
    detail::http_upgrade_protocols upgrade_protocols_;
    std::string_view first_upgrade_value_{};
    std::size_t upgrade_value_bytes_{0};
    std::size_t upgrade_field_count_{0};
    bool has_te_{false};
    // The caller declared trailer fields; the writer requires chunked content.
    bool has_trailer_{false};
};

[[nodiscard]] bool analyze_http1_client_request_field(http_header_view header, detail::http_field_kind kind,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source = http_request_field_source::generated) noexcept;

[[nodiscard]] bool finish_http1_client_request_fields(
    const http1_client_request_field_facts& facts, http1_client_request_prepare_error& error) noexcept;

// Validate every field and collect the facts the encoder needs. Returns false
// with `error` set on the first field that cannot be sent.
[[nodiscard]] bool analyze_http1_client_request_fields(std::span<const http_header_view> headers,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

struct http1_client_request_prepare_result_access final {
    using result_type = std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>;

    [[nodiscard]] static http1_client_exchange_state exchange_state(http_known_method method,
        http1_close_policy close_policy, http1_client_initial_content_state content_state,
        std::string offered_upgrade_protocols) noexcept {
        return http1_client_exchange_state(
            method, close_policy, content_state, std::move(offered_upgrade_protocols));
    }

    [[nodiscard]] static constexpr result_type buffer_too_small(
        std::size_t required_head_bytes) noexcept {
        return result_type(http1_client_request_buffer_too_small(required_head_bytes));
    }

    [[nodiscard]] static constexpr result_type failure(
        http1_client_request_prepare_error error) noexcept {
        return result_type(error);
    }

    [[nodiscard]] static result_type prepared_streaming_content(
        std::string_view head, std::optional<std::uint64_t> length, bool gated,
        http1_client_exchange_state state_value) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_streaming_request_content(length, gated)), std::move(state_value)));
    }

    [[nodiscard]] static result_type prepared_without_content(
        std::string_view head, http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_request_without_content()),
            std::move(exchange_state)));
    }

    [[nodiscard]] static result_type prepared_immediate_content(
        std::string_view head, std::string_view content_bytes,
        http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_immediate_request_content(content_bytes)),
            std::move(exchange_state)));
    }

    [[nodiscard]] static result_type prepared_continue_gated_content(
        std::string_view head, std::string_view content_bytes,
        http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_continue_gated_request_content(content_bytes)),
            std::move(exchange_state)));
    }
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

constexpr std::string_view http11_request_line_suffix = " HTTP/1.1\r\n";
constexpr std::string_view host_prefix = "Host: ";
constexpr std::string_view content_length_prefix = "Content-Length: ";
constexpr std::string_view expect_prefix = "Expect: ";
constexpr std::string_view connection_close = "Connection: close\r\n";
constexpr std::string_view chunked_header = "Transfer-Encoding: chunked\r\n";

void append_authority(detail::http_raw_wire_sink& sink, std::string_view host, std::string_view port) noexcept {
    sink.append(host);
    if (!port.empty()) {
        sink.append(':');
        sink.append(port);
    }
}

struct request_head_write_plan final {
    std::string_view host_;
    std::string_view port_;
    std::string_view method_;
    // "http://" or "https://" ahead of the authority for absolute-form;
    // empty for origin-form.
    std::string_view absolute_scheme_;
    std::string_view target_;
    std::span<const http_header_view> headers_;
    std::string_view content_length_;
    bool connect_;
    bool explicit_content_;
    bool expect_continue_;
    bool generate_connection_close_;
};

[[nodiscard]] char* write_request_head(char* output, const request_head_write_plan& plan) noexcept {
    detail::http_raw_wire_sink sink(output);
    sink.append(plan.method_);
    sink.append(' ');
    if (plan.connect_) {
        append_authority(sink, plan.host_, plan.port_);
    } else {
        if (!plan.absolute_scheme_.empty()) {
            sink.append(plan.absolute_scheme_);
            append_authority(sink, plan.host_, plan.port_);
        }
        sink.append(plan.target_);
    }
    sink.append(http11_request_line_suffix);
    sink.append(host_prefix);
    append_authority(sink, plan.host_, plan.port_);
    sink.append(crlf);
    for (const auto& header : plan.headers_) {
        detail::emit_http1_field_line(sink, header.name(), header.value());
    }
    if (plan.explicit_content_ && plan.content_length_.empty()) {
        sink.append(chunked_header);
    } else if (plan.explicit_content_) {
        sink.append(content_length_prefix);
        sink.append(plan.content_length_);
        sink.append(crlf);
    }
    if (plan.expect_continue_) {
        sink.append(expect_prefix);
        sink.append(detail::http_continue_expectation_token);
        sink.append(crlf);
    }
    if (plan.generate_connection_close_) {
        sink.append(connection_close);
    }
    sink.append(crlf);
    return sink.position();
}

[[nodiscard]] bool is_valid_http1_close_policy(http1_close_policy policy) noexcept {
    switch (policy) {
        case http1_close_policy::allow_reuse:
        case http1_close_policy::close_after_response:
            return true;
    }
    return false;
}

[[nodiscard]] bool is_valid_http_client_request_expectation(
    http_client_request_expectation expectation) noexcept {
    switch (expectation) {
        case http_client_request_expectation::none:
        case http_client_request_expectation::continue_value:
            return true;
    }
    return false;
}

[[nodiscard]] std::string own_offered_upgrade_protocols(
    std::span<const http_header_view> headers, const detail::http1_client_request_field_facts& facts) {
    if (facts.upgrade_field_count_ == 0) {
        return {};
    }
    if (facts.upgrade_field_count_ == 1) {
        return std::string(facts.first_upgrade_value_);
    }
    std::string result;
    result.reserve(facts.upgrade_value_bytes_);
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "Upgrade")) {
            continue;
        }
        if (!result.empty()) {
            result.push_back(',');
        }
        result.append(header.value());
    }
    return result;
}

// Every request line, CONNECT included, rejects these wire policies first.
// CONNECT is always authority-form (RFC 9112 section 3.2.3).
[[nodiscard]] std::optional<http1_client_request_prepare_error> validate_wire_policy(
    http1_client_request_wire_policy policy, bool connect) noexcept {
    if (!is_valid_http1_close_policy(policy.close_policy_)) {
        return http1_client_request_prepare_error::invalid_close_policy;
    }
    if (!is_valid_http_client_request_expectation(policy.expectation_)) {
        return http1_client_request_prepare_error::invalid_expectation;
    }
    switch (policy.target_form_) {
        case http1_request_target_form::origin:
            return std::nullopt;
        case http1_request_target_form::absolute:
            if (!connect) {
                return std::nullopt;
            }
            break;
    }
    return http1_client_request_prepare_error::invalid_target_form;
}

[[nodiscard]] std::optional<http1_client_request_prepare_error> validate_non_connect_request_line(
    std::string_view method, std::string_view target, http1_client_request_wire_policy policy,
    http_known_method& known_method) noexcept {
    if (const auto error = validate_wire_policy(policy, false)) {
        return error;
    }
    if (!is_valid_http_method_token(method)) {
        return http1_client_request_prepare_error::invalid_method;
    }
    known_method = classify_http_method(method);
    if (known_method == http_known_method::connect) {
        return http1_client_request_prepare_error::connect_requires_dedicated_entry;
    }
    if (!detail::is_valid_origin_or_asterisk_form_target(known_method, target)) {
        return http1_client_request_prepare_error::invalid_target;
    }
    return std::nullopt;
}

[[nodiscard]] detail::http1_client_request_prepare_result_access::result_type prepare_request(const http_origin_view& origin,
    std::string_view method, http_known_method known_method, std::string_view target,
    std::span<const http_header_view> headers, http_client_request_content_view content,
    std::span<char> head_buffer, http1_client_request_wire_policy policy,
    const http1_client_request_head_view* streaming = nullptr,
    std::span<const http_header_view> connect_udp_headers = {}) {
    if (!detail::is_valid_http_request_field_source(policy.field_source_)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_header);
    }
    const bool connect = known_method == http_known_method::connect;
    detail::http1_client_request_field_facts header_facts;
    http1_client_request_prepare_error error = http1_client_request_prepare_error::invalid_header;
    const auto* content_bytes = content.borrowed_bytes();
    const bool explicit_content = content_bytes != nullptr || streaming != nullptr;
    const auto length = streaming       ? (streaming->content_length_
                                                  ? std::optional<std::uint64_t>(streaming->content_length_->value())
                                                  : std::nullopt)
                        : content_bytes ? std::optional<std::uint64_t>(content_bytes->value().size())
                                        : std::nullopt;
    const bool chunked = streaming && !length;
    if (!detail::analyze_http1_client_request_fields(headers, header_facts, error, policy.field_source_)) {
        return detail::http1_client_request_prepare_result_access::failure(error);
    }
    // A sender that will generate trailer fields SHOULD declare them (RFC 9110
    // section 6.6.2); only chunked content has a trailer section.
    if (header_facts.has_trailer_ && !chunked) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::trailer_requires_chunked_content);
    }
    const bool expect_continue = policy.expectation_ == http_client_request_expectation::continue_value;
    const auto content_indication = explicit_content && (!length || *length != 0)
                                        ? http_request_content_indication::will_follow
                                        : http_request_content_indication::no_content;
    if (!http_client_expectation_is_valid(expect_continue, content_indication)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::expectation_without_content);
    }
    if (content_indication == http_request_content_indication::will_follow) {
        const auto content_semantics = classify_http_request_content_semantics(method);
        if (content_semantics == http_request_content_semantics::forbidden) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::content_forbidden_for_method);
        }
        // RFC 9110 section 9.3.7 requires the media description when generating
        // OPTIONS content, not when forwarding an already received request.
        if (policy.field_source_ == http_request_field_source::generated &&
            content_semantics == http_request_content_semantics::content_type_required &&
            !header_facts.fields_.seen(detail::http_field_kind::content_type)) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::options_content_type_required);
        }
    }

    const bool generate_connection_close =
        policy.close_policy_ == http1_close_policy::close_after_response &&
        !header_facts.connection_options_.close();
    const auto effective_close_policy =
        header_facts.connection_options_.close() || generate_connection_close
            ? http1_close_policy::close_after_response
            : http1_close_policy::allow_reuse;

    // Reuse each decimal token in measurement and emission. CONNECT uses the
    // same port token for both its authority target and generated Host field.
    std::array<char, http_max_unsigned_decimal_size> port_buffer;
    const auto port_value = connect || origin.port() != (origin.scheme() == http_scheme::https ? std::uint16_t{443} : std::uint16_t{80})
                                ? http_format_unsigned(port_buffer, origin.port())
                                : std::string_view{};
    const auto authority_bytes = origin.host().size() + (port_value.empty() ? 0 : 1 + port_value.size());
    std::array<char, http_max_unsigned_decimal_size> content_length_buffer;
    const auto content_length_value = explicit_content && !chunked
                                          ? http_format_unsigned(content_length_buffer, *length)
                                          : std::string_view{};
    // absolute-form names the origin as the generated Host does, so the two
    // authorities agree (RFC 9112 section 3.2); OPTIONS * keeps only the
    // authority, with an empty path (section 3.2.4).
    const auto absolute_scheme = connect || policy.target_form_ != http1_request_target_form::absolute
                                     ? std::string_view{}
                                 : origin.scheme() == http_scheme::https ? std::string_view{"https://"}
                                                                         : std::string_view{"http://"};
    const auto emitted_target = !absolute_scheme.empty() && target == "*" ? std::string_view{} : target;
    const std::size_t target_bytes =
        connect ? authority_bytes
                : (absolute_scheme.empty() ? 0 : absolute_scheme.size() + authority_bytes) + emitted_target.size();
    // The caller's fields are already charged; add the request line and the
    // generated fields to the same section budget.
    auto head_budget = header_facts.section_;
    if (!head_budget.add(method.size()) || !head_budget.add(1) || !head_budget.add(target_bytes) ||
        !head_budget.add(http11_request_line_suffix.size()) ||
        !head_budget.add(host_prefix.size()) || !head_budget.add(authority_bytes) ||
        !head_budget.add(crlf.size()) || (chunked && !head_budget.add(chunked_header.size())) ||
        (explicit_content && !chunked &&
            (!head_budget.add(content_length_prefix.size()) ||
                !head_budget.add(content_length_value.size()) || !head_budget.add(crlf.size()))) ||
        (expect_continue &&
            (!head_budget.add(expect_prefix.size()) ||
                !head_budget.add(detail::http_continue_expectation_token.size()) ||
                !head_budget.add(crlf.size()))) ||
        (generate_connection_close && !head_budget.add(connection_close.size())) ||
        !head_budget.add(crlf.size())) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::header_too_large);
    }
    if (!connect_udp_headers.empty() &&
        validate_http_connect_udp_request_to_send({
                                                      .version_ = http_protocol_version::http11,
                                                      .method_ = "GET",
                                                      .authority_ = origin.host(),
                                                      .path_ = target,
                                                      .headers_ = connect_udp_headers,
                                                  })
            .has_value()) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_header);
    }
    const auto head_bytes = head_budget.bytes();
    if (head_buffer.size() < head_bytes) {
        return detail::http1_client_request_prepare_result_access::buffer_too_small(head_bytes);
    }

    // Upgrade is the only response-planning fact that cannot be reduced to a
    // fixed-size value. Own it before mutating the caller's output buffer so an
    // allocation failure leaves request preparation transactional.
    auto offered_upgrade_protocols = header_facts.upgrade_protocols_.has_protocol()
                                         ? own_offered_upgrade_protocols(headers, header_facts)
                                         : std::string();

    const auto content_state = expect_continue
                                   ? detail::http1_client_initial_content_state::awaiting_continue
                                   : (explicit_content && (!length || *length != 0)
                                             ? detail::http1_client_initial_content_state::pending
                                             : detail::http1_client_initial_content_state::complete);
    auto exchange_state = detail::http1_client_request_prepare_result_access::exchange_state(
        known_method, effective_close_policy, content_state,
        std::move(offered_upgrade_protocols));
    const request_head_write_plan write_plan{origin.host(), port_value, method, absolute_scheme, emitted_target, headers, content_length_value,
        connect, explicit_content, expect_continue, generate_connection_close};
    const auto output = head_buffer.first(head_bytes);
    // Descriptors and every borrowed string are consumed before the output is
    // overwritten when the caller's request storage aliases the head buffer.
    detail::write_http_wire(output,
        detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), headers,
            {method, target, origin.host()}),
        [&write_plan](char* destination) noexcept { return write_request_head(destination, write_plan); });
    const auto head = std::string_view(head_buffer.data(), head_bytes);
    if (streaming) {
        return detail::http1_client_request_prepare_result_access::prepared_streaming_content(head, length, expect_continue, std::move(exchange_state));
    }
    if (!explicit_content) {
        return detail::http1_client_request_prepare_result_access::prepared_without_content(
            head, std::move(exchange_state));
    }
    if (expect_continue) {
        return detail::http1_client_request_prepare_result_access::prepared_continue_gated_content(
            head, content_bytes->value(), std::move(exchange_state));
    }
    return detail::http1_client_request_prepare_result_access::prepared_immediate_content(
        head, content_bytes->value(), std::move(exchange_state));
}

// The one CONNECT rule set behind both prepare_connect() entries: wire-policy
// checks first, then the tunnel target (null when the caller's authority is
// invalid; port 0 is never a tunnel target), emitted in authority-form.
[[nodiscard]] detail::http1_client_request_prepare_result_access::result_type prepare_connect_request(
    const http_origin_view* tunnel_origin, std::span<const http_header_view> headers, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) {
    if (const auto error = validate_wire_policy(policy, true)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    if (tunnel_origin == nullptr || tunnel_origin->port() == 0) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_connect_origin);
    }
    return prepare_request(*tunnel_origin, "CONNECT", http_known_method::connect, {}, headers,
        http_client_request_content_view::none(), head_buffer, policy);
}

}  // namespace

std::string_view http1_client_request_prepare_error_message(
    http1_client_request_prepare_error error) noexcept {
    switch (error) {
        case http1_client_request_prepare_error::invalid_method:
            return "invalid HTTP/1 client request method";
        case http1_client_request_prepare_error::invalid_target:
            return "invalid HTTP/1 client request target";
        case http1_client_request_prepare_error::connect_requires_dedicated_entry:
            return "CONNECT requires the dedicated HTTP/1 client entry";
        case http1_client_request_prepare_error::invalid_connect_origin:
            return "invalid HTTP/1 CONNECT origin";
        case http1_client_request_prepare_error::invalid_header:
            return "invalid HTTP/1 client request header";
        case http1_client_request_prepare_error::host_header_managed_by_writer:
            return "HTTP/1 client Host is managed by the writer";
        case http1_client_request_prepare_error::content_length_managed_by_writer:
            return "HTTP/1 client Content-Length is managed by the writer";
        case http1_client_request_prepare_error::transfer_encoding_unsupported:
            return "HTTP/1 client Transfer-Encoding is unsupported";
        case http1_client_request_prepare_error::trailer_requires_chunked_content:
            return "HTTP/1 client Trailer requires chunked request content";
        case http1_client_request_prepare_error::expect_header_managed_by_writer:
            return "HTTP/1 client Expect is managed by the writer";
        case http1_client_request_prepare_error::invalid_connection:
            return "invalid HTTP/1 client Connection header";
        case http1_client_request_prepare_error::invalid_upgrade:
            return "invalid HTTP/1 client Upgrade header";
        case http1_client_request_prepare_error::upgrade_connection_option_required:
            return "HTTP/1 Upgrade requires Connection: Upgrade";
        case http1_client_request_prepare_error::te_connection_option_required:
            return "HTTP/1 TE requires Connection: TE";
        case http1_client_request_prepare_error::expectation_without_content:
            return "Continue expectation requires non-empty request content";
        case http1_client_request_prepare_error::content_forbidden_for_method:
            return "request method forbids content";
        case http1_client_request_prepare_error::options_content_type_required:
            return "OPTIONS content requires Content-Type";
        case http1_client_request_prepare_error::header_too_large:
            return "HTTP/1 client request header is too large";
        case http1_client_request_prepare_error::invalid_close_policy:
            return "invalid HTTP/1 client close policy";
        case http1_client_request_prepare_error::invalid_expectation:
            return "invalid HTTP/1 client request expectation";
        case http1_client_request_prepare_error::invalid_target_form:
            return "invalid HTTP/1 client request target form";
    }
    return "invalid HTTP/1 client request";
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare(const http_origin_view& origin,
    const http_client_request_view& request, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line(
            request.method_, request.target_, policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    return prepare_request(origin, request.method_, known_method, request.target_,
        static_cast<std::span<const http_header_view>>(request.headers_), request.content_, head_buffer,
        policy);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_streaming(const http_origin_view& origin,
    const http1_client_request_head_view& request, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line(
            request.method_, request.target_, policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    return prepare_request(origin, request.method_, known_method, request.target_, request.headers_,
        http_client_request_content_view::none(), head_buffer, policy, &request);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_connect_udp(const http_origin_view& origin,
    borrowed_text target, std::span<const http_header_view> headers, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line("GET", target.view(), policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "connection")) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::invalid_connection);
        }
        if (http_ascii_equals_ignore_case(field.name(), "upgrade")) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::invalid_upgrade);
        }
    }
    detail::http_header_descriptor_storage<http_header_view> staged_fields;
    staged_fields.reserve(headers.size() + 3U);
    for (const auto& field : headers) {
        staged_fields.append(field);
    }
    staged_fields.append(
        http_header_view{detail::http_connection_field_name, detail::http_connection_upgrade_option});
    staged_fields.append(http_header_view{"Upgrade", "connect-udp"});
    staged_fields.append(http_header_view{"Host", origin.host()});
    const auto all_fields = staged_fields.view();
    const auto emitted_headers = all_fields.first(all_fields.size() - 1U);
    return prepare_request(origin, "GET", known_method, target.view(), emitted_headers,
        http_client_request_content_view::none(), head_buffer, policy, nullptr, all_fields);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_connect(
    const http_origin_view& tunnel_origin, std::span<const http_header_view> headers,
    std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    return prepare_connect_request(&tunnel_origin, headers, head_buffer, policy);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_connect(borrowed_text authority,
    std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    // RFC 9110 section 9.3.6: the tunnel destination needs a non-empty port,
    // and port zero is rejected by prepare_connect_request.
    const auto parsed_value = parse_http_authority(authority);
    if (!parsed_value || parsed_value->port_kind() != http_authority_port_kind::value) {
        return prepare_connect_request(nullptr, headers, head_buffer, policy);
    }
    const auto tunnel_origin =
        http_origin_view::http({.host_ = parsed_value->host(), .port_ = *parsed_value->port()});
    return prepare_connect_request(&tunnel_origin, headers, head_buffer, policy);
}

http1_response_head_plan prepare_http1_connect_response_head(
    const http_response& response, http_protocol_version version) {
    if ((version != http_protocol_version::http10 && version != http_protocol_version::http11) ||
        !response.status().is_successful() || response.file_body() || !response.body_bytes().empty()) {
        throw http1_response_head_error("invalid HTTP/1 CONNECT response head");
    }
    if (detail::response_has_known_header(
            response, detail::response_header_content_length | detail::response_header_transfer_encoding)) {
        throw http1_response_head_error("CONNECT response cannot contain message framing fields");
    }
    const auto body = plan_http_response_body(http_known_method::connect, response.status());
    const auto connection = version == http_protocol_version::http10
                                ? plan_http10_request_connection(true, false)
                                : http1_request_connection_plan::http11_close();
    return http1_close_delimited_response_stream_head_plan(body, connection);
}

}  // namespace ruvia

namespace ruvia::detail {

bool analyze_http1_client_request_field(http_header_view header, detail::http_field_kind kind,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source) noexcept {
    const auto name = header.name();
    const auto value = header.value();
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        error = http1_client_request_prepare_error::invalid_header;
        return false;
    }
    // The HTTP/1 writer owns Host and refuses message framing it does not
    // generate; the remaining semantics are version-independent.
    switch (kind) {
        case detail::http_field_kind::host:
            error = http1_client_request_prepare_error::host_header_managed_by_writer;
            return false;
        case detail::http_field_kind::transfer_encoding:
            error = http1_client_request_prepare_error::transfer_encoding_unsupported;
            return false;
        default:
            break;
    }
    switch (detail::validate_request_field(kind, value, detail::http_field_list_role::sender, facts.fields_, source)) {
        case detail::request_field_status::ok:
            break;
        case detail::request_field_status::invalid:
            error = http1_client_request_prepare_error::invalid_header;
            return false;
        case detail::request_field_status::managed_by_sender:
            error = kind == detail::http_field_kind::expect
                        ? http1_client_request_prepare_error::expect_header_managed_by_writer
                        : http1_client_request_prepare_error::content_length_managed_by_writer;
            return false;
    }
    if (kind == detail::http_field_kind::connection) {
        if (facts.connection_options_.parse_field(value, detail::http_connection_field_role::sender) !=
            detail::http_field_list_parse_status::ok) {
            error = http1_client_request_prepare_error::invalid_connection;
            return false;
        }
    } else if (kind == detail::http_field_kind::upgrade) {
        const auto role = source == http_request_field_source::forwarded
                              ? detail::http_field_list_role::recipient
                              : detail::http_field_list_role::sender;
        if (facts.upgrade_protocols_.parse_field(value, role) !=
            detail::http_field_list_parse_status::ok) {
            error = http1_client_request_prepare_error::invalid_upgrade;
            return false;
        }
        if (facts.upgrade_field_count_ == 0) {
            facts.first_upgrade_value_ = value;
        } else {
            ++facts.upgrade_value_bytes_;
        }
        facts.upgrade_value_bytes_ += value.size();
        ++facts.upgrade_field_count_;
    } else if (kind == detail::http_field_kind::te) {
        facts.has_te_ = true;
    } else if (kind == detail::http_field_kind::trailer) {
        facts.has_trailer_ = true;
    }

    if (!facts.section_.add_field(name, value)) {
        error = http1_client_request_prepare_error::header_too_large;
        return false;
    }
    return true;
}

bool finish_http1_client_request_fields(
    const http1_client_request_field_facts& facts, http1_client_request_prepare_error& error) noexcept {
    if (facts.upgrade_protocols_.has_field() && !facts.connection_options_.upgrade()) {
        error = http1_client_request_prepare_error::upgrade_connection_option_required;
        return false;
    }
    if (facts.has_te_ && !facts.connection_options_.te()) {
        error = http1_client_request_prepare_error::te_connection_option_required;
        return false;
    }
    return true;
}

bool analyze_http1_client_request_fields(std::span<const http_header_view> headers, http1_client_request_field_facts& facts,
    http1_client_request_prepare_error& error, http_request_field_source source) noexcept {
    for (const auto& header : headers) {
        if (!analyze_http1_client_request_field(header, detail::classify_http_field(header.name()), facts, error, source)) {
            return false;
        }
    }
    return finish_http1_client_request_fields(facts, error);
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

[[nodiscard]] char* write_chunked_trailers(char* output, std::span<const http_header_view> trailers) noexcept {
    detail::http_raw_wire_sink sink(output);
    sink.append(http1_last_chunk_prefix);
    for (const auto& field : trailers) {
        detail::emit_http1_field_line(sink, field.name(), field.value());
    }
    sink.append(http1_trailer_section_terminator);
    return sink.position();
}

}  // namespace

using write_error_type = http1_request_content_write_error;
http1_request_content_writer::http1_request_content_writer(const http1_client_streaming_request_content& plan) noexcept
    : length_(plan.content_length()),
      gated_(plan.continue_gated()) {}
void http1_request_content_writer::release_content() noexcept {
    gated_ = false;
}
void http1_request_content_writer::abort() noexcept {
    stopped_ = true;
    writing_ = false;
    finishing_ = false;
}
std::variant<http1_request_content_writer::chunk, write_error_type> http1_request_content_writer::plan_chunk(std::span<const char> payload_value) noexcept {
    if (stopped_ || finished_) {
        return write_error_type::stopped;
    }
    if (gated_) {
        return write_error_type::awaiting_continue;
    }
    if (writing_ || finishing_) {
        return write_error_type::write_pending;
    }
    if (payload_value.size() > std::numeric_limits<std::uint64_t>::max() - committed_) {
        return write_error_type::length_overflow;
    }
    if (length_ && payload_value.size() > *length_ - committed_) {
        return write_error_type::length_mismatch;
    }
    chunk result;
    result.payload_ = payload_value;
    if (!length_ && !payload_value.empty()) {
        http1_chunk_header header_value(payload_value.size());
        auto prefix = header_value.view();
        std::copy(prefix.begin(), prefix.end(), result.prefix_.begin());
        result.prefix_size_ = prefix.size();
        result.suffix_ = http1_chunk_data_terminator;
    }
    writing_ = true;
    pending_ = payload_value.size();
    return result;
}
void http1_request_content_writer::commit_chunk() {
    if (stopped_ || finished_ || !writing_) {
        throw std::logic_error("HTTP/1 request content chunk commit requires a pending chunk write");
    }
    committed_ += pending_;
    pending_ = 0;
    writing_ = false;
}
std::variant<std::string_view, write_error_type> http1_request_content_writer::plan_finish(std::span<char> buffer, std::span<const http_header_view> trailers) noexcept {
    if (stopped_ || finished_) {
        return write_error_type::stopped;
    }
    if (gated_) {
        return write_error_type::awaiting_continue;
    }
    if (writing_ || finishing_) {
        return write_error_type::write_pending;
    }
    if (length_ && committed_ != *length_) {
        return write_error_type::length_mismatch;
    }
    if (length_ && !trailers.empty()) {
        return write_error_type::trailers_require_chunked;
    }
    // The last-chunk line shares the section budget, which keeps the whole
    // encoded ending within the bounded wire snapshot.
    auto section = http_trailer_section_budget();
    (void)section.add(http1_last_chunk_prefix.size());
    for (const auto& field : trailers) {
        if (const auto error = detail::charge_http_request_trailer_field(section, field.name(), field.value())) {
            return *error == http_request_trailer_error::section_too_large ? write_error_type::header_too_large
                                                                           : write_error_type::invalid_trailer;
        }
    }
    const auto required = length_ ? std::size_t{0} : section.bytes();
    if (buffer.size() < required) {
        return write_error_type::output_too_small;
    }
    if (!length_) {
        const auto output = buffer.first(required);
        detail::write_http_wire(output,
            detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), trailers),
            [trailers](char* destination) noexcept { return write_chunked_trailers(destination, trailers); });
    }
    finishing_ = true;
    return required ? std::string_view(buffer.data(), required) : std::string_view{};
}
void http1_request_content_writer::commit_finish() {
    if (stopped_ || finished_ || !finishing_) {
        throw std::logic_error("HTTP/1 request content finish commit requires a pending finish write");
    }
    finishing_ = false;
    finished_ = true;
}
}  // namespace ruvia

namespace ruvia {
namespace {
// The one application-header policy of the client handshake: the request
// writer owns every protocol-managed field (Host, framing, Expect and the
// Upgrade/Connection pair) and the handshake generates every Sec-WebSocket-*
// field.
bool is_reserved_handshake_header(std::string_view name, detail::http_field_kind kind) noexcept {
    switch (kind) {
        case detail::http_field_kind::sec_websocket_key:
        case detail::http_field_kind::sec_websocket_version:
        case detail::http_field_kind::sec_websocket_protocol:
        case detail::http_field_kind::sec_websocket_extensions:
            return true;
        case detail::http_field_kind::other:
            return detail::is_protocol_managed_request_field(kind, name) ||
                   http_ascii_equals_ignore_case(name, "sec-websocket-accept");
        default:
            return detail::is_protocol_managed_request_field(kind, name);
    }
}
}  // namespace

void validate_http1_websocket_client_handshake_configuration(std::span<const http_header_view> headers) {
    detail::http1_client_request_field_facts facts;
    http1_client_request_prepare_error error{};
    for (const auto& header : headers) {
        const auto kind = detail::classify_http_field(header.name());
        if (is_reserved_handshake_header(header.name(), kind)) {
            throw std::invalid_argument("invalid or reserved WebSocket handshake header");
        }
        if (!detail::analyze_http1_client_request_field(header, kind, facts, error)) {
            throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
        }
    }
    if (!detail::finish_http1_client_request_fields(facts, error)) {
        throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
    }
}

namespace {
template <typename header_type>
std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_websocket_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const header_type> headers, std::span<char> head_buffer, http_request_field_source source) {
    const auto negotiation_fields = negotiation.request_fields();
    detail::http_header_descriptor_storage<http_header_view> staged_headers;
    staged_headers.reserve(headers.size() + 3U + negotiation_fields.size());
    for (const auto& header : headers) {
        staged_headers.append(http_header_view{header.name(), header.value()});
    }
    staged_headers.append(http_header_view{"Upgrade", "websocket"});
    staged_headers.append(
        http_header_view{detail::http_connection_field_name, detail::http_connection_upgrade_option});
    staged_headers.append(
        http_header_view{"Sec-WebSocket-Key", std::string_view(encoded_key.data(), encoded_key.size())});
    for (const auto& field : negotiation_fields) {
        staged_headers.append(http_header_view{field.name(), field.value()});
    }
    return http1_client_request_writer()
        .prepare(origin, {.method_ = "GET", .target_ = target, .headers_ = staged_headers.view()}, head_buffer, {.field_source_ = source});
}

}  // namespace

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header> headers, std::span<char> head_buffer, http_request_field_source source) {
    return prepare_websocket_request(origin, target, encoded_key, negotiation, headers, head_buffer, source);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header_view> headers, std::span<char> head_buffer, http_request_field_source source) {
    return prepare_websocket_request(origin, target, encoded_key, negotiation, headers, head_buffer, source);
}

std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24> encoded_key,
    const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response) {
    if (response.plan().protocol_upgrade() == nullptr ||
        response.head().status() != http_status::switching_protocols) {
        return websocket_client_handshake_error::response_status;
    }
    detail::websocket_accept_key_type expected{};
    detail::encode_websocket_accept(
        expected, std::string_view(encoded_key.data(), encoded_key.size()));
    std::size_t accepts = 0;
    bool accept_matches = false, has_upgrade = false, has_connection = false;
    for (const auto& header : response.head().headers()) {
        switch (detail::classify_http_field(header.name())) {
            case detail::http_field_kind::upgrade:
                has_upgrade = has_upgrade || detail::http_has_token(header.value(), "websocket");
                break;
            case detail::http_field_kind::connection:
                has_connection = has_connection ||
                                 detail::http_has_token(header.value(), detail::http_connection_upgrade_option);
                break;
            case detail::http_field_kind::other:
                if (http_ascii_equals_ignore_case(header.name(), "Sec-WebSocket-Accept")) {
                    ++accepts;
                    accept_matches = http_trim_ows(header.value()) ==
                                     std::string_view(expected.data(), expected.size());
                }
                break;
            default:
                break;
        }
    }
    if (accepts != 1 || !accept_matches) {
        return websocket_client_handshake_error::accept;
    }
    if (!has_upgrade) {
        return websocket_client_handshake_error::upgrade;
    }
    if (!has_connection) {
        return websocket_client_handshake_error::connection;
    }

    return detail::validate_websocket_client_selection(negotiation, response.head().headers(), false);
}
}  // namespace ruvia

namespace ruvia::detail {

struct http1_client_response_parse_result_access final {
    [[nodiscard]] static http1_client_response_parse_result need_more() noexcept {
        return http1_client_response_parse_result(http1_client_response_need_more());
    }

    [[nodiscard]] static http1_client_response_parse_result failure(
        http1_client_response_parse_error error) noexcept {
        return http1_client_response_parse_result(error);
    }

    [[nodiscard]] static http1_client_response_parse_result parsed(http_client_response_head head,
        http1_client_response_plan plan, std::size_t consumed_bytes) noexcept {
        return http1_client_response_parse_result(
            http1_parsed_client_response_head(std::move(head), std::move(plan), consumed_bytes));
    }
};

}  // namespace ruvia::detail

namespace ruvia {

namespace {

[[nodiscard]] constexpr detail::http1_client_request_content_phase receive_continue(
    detail::http1_client_request_content_phase phase) noexcept {
    switch (phase) {
        case detail::http1_client_request_content_phase::awaiting_continue:
        case detail::http1_client_request_content_phase::content_released_awaiting_continue:
            return detail::http1_client_request_content_phase::continue_received;
        case detail::http1_client_request_content_phase::content_complete_awaiting_continue:
            return detail::http1_client_request_content_phase::continue_received_content_complete;
        case detail::http1_client_request_content_phase::content_complete:
        case detail::http1_client_request_content_phase::content_pending:
        case detail::http1_client_request_content_phase::continue_received:
        case detail::http1_client_request_content_phase::continue_received_content_complete:
            return phase;
    }
    return phase;
}

}  // namespace

std::string_view http1_client_response_parse_error_message(
    http1_client_response_parse_error error) noexcept {
    switch (error) {
        case http1_client_response_parse_error::header_too_large:
            return "response header is too large";
        case http1_client_response_parse_error::invalid_status_line:
            return "invalid response status line";
        case http1_client_response_parse_error::unsupported_http_version:
            return "unsupported response HTTP version";
        case http1_client_response_parse_error::invalid_status_code:
            return "invalid response status code";
        case http1_client_response_parse_error::invalid_reason_phrase:
            return "invalid response reason phrase";
        case http1_client_response_parse_error::invalid_header:
            return "invalid response header";
        case http1_client_response_parse_error::invalid_connection:
            return "invalid response Connection header";
        case http1_client_response_parse_error::invalid_upgrade:
            return "invalid response Upgrade header";
        case http1_client_response_parse_error::invalid_content_length:
            return "invalid response Content-Length";
        case http1_client_response_parse_error::conflicting_content_length:
            return "conflicting response Content-Length";
        case http1_client_response_parse_error::invalid_transfer_encoding:
            return "invalid response Transfer-Encoding";
        case http1_client_response_parse_error::unsupported_transfer_encoding:
            return "unsupported response transfer coding";
        case http1_client_response_parse_error::content_length_and_transfer_encoding:
            return "response has both Content-Length and Transfer-Encoding";
        case http1_client_response_parse_error::invalid_protocol_switch:
            return "invalid Switching Protocols response";
    }
    return "invalid HTTP/1 response";
}

detail::http1_client_request_content_phase
http1_client_response_parser::initial_request_content_phase(
    const http1_client_exchange_state& state_value) noexcept {
    switch (detail::http1_client_exchange_state_access::content_state(state_value)) {
        case detail::http1_client_initial_content_state::complete:
            return detail::http1_client_request_content_phase::content_complete;
        case detail::http1_client_initial_content_state::pending:
            return detail::http1_client_request_content_phase::content_pending;
        case detail::http1_client_initial_content_state::awaiting_continue:
            return detail::http1_client_request_content_phase::awaiting_continue;
    }
    return detail::http1_client_request_content_phase::content_complete;
}

http1_client_response_parser::http1_client_response_parser(
    http1_client_exchange_state exchange_state) noexcept
    : exchange_state_(std::move(exchange_state)),
      request_content_phase_(initial_request_content_phase(exchange_state_)) {}

http1_client_response_parse_result http1_client_response_parser::parse(std::string_view buffer) {
    if (phase_ != phase_type::await_response) {
        throw std::logic_error("HTTP/1 client response parser is already terminal");
    }
    const auto fail = [this](http1_client_response_parse_error error) noexcept {
        phase_ = phase_type::failed;
        return detail::http1_client_response_parse_result_access::failure(error);
    };

    // The status line is validated once its line feed arrives, so a non-HTTP
    // peer or a bare LF fails without waiting for the whole head. Bytes before
    // header_scan_offset_ held no line feed in an earlier call.
    if (!status_line_) {
        const auto line_feed = buffer.substr(0, max_http_header_bytes).find('\n', header_scan_offset_);
        if (line_feed != std::string_view::npos) {
            if (line_feed == 0 || buffer[line_feed - 1] != '\r') {
                return fail(http1_client_response_parse_error::invalid_status_line);
            }
            const auto status_line = detail::parse_http1_client_status_line(buffer.substr(0, line_feed - 1));
            if (status_line.index() != 0) {
                return fail(std::get<1>(status_line));
            }
            status_line_ = std::get<0>(status_line);
            status_line_end_ = line_feed - 1;
        }
    }

    const auto header_bytes = detail::find_http_header_end(buffer, header_scan_offset_);
    if (header_bytes == std::string_view::npos) {
        // A field line ending in a bare LF can never complete the
        // CRLF-delimited head (RFC 9112 section 2.2).
        if (status_line_ &&
            detail::has_http_bare_line_feed(buffer, std::max(status_line_end_ + 2, header_scan_offset_))) {
            return fail(http1_client_response_parse_error::invalid_header);
        }
        if (buffer.size() >= max_http_header_bytes) {
            return fail(http1_client_response_parse_error::header_too_large);
        }
        header_scan_offset_ = buffer.size() > 3 ? buffer.size() - 3 : 0;
        return detail::http1_client_response_parse_result_access::need_more();
    }
    // A complete head holds a line feed, so its status line was validated
    // above unless the input broke the same-prefix contract of parse().
    // A retry after an allocation failure rescans from the start.
    if (!status_line_) {
        throw std::logic_error("HTTP/1 client response input does not extend the previous prefix");
    }
    const auto status_line = *status_line_;
    const auto fields_begin = status_line_end_ + 2;
    header_scan_offset_ = 0;
    status_line_.reset();
    status_line_end_ = 0;

    // Exclude only the final empty line: every field keeps its CRLF, matching
    // the request scanner even when the head contains no fields.
    auto field_section = buffer.substr(fields_begin, header_bytes - 2 - fields_begin);
    std::optional<std::string> unfolded_fields;
    // The status line is not a field: its following whitespace cannot be
    // folded into the reason phrase. Only normalize within the field block.
    if (const auto first_fold = detail::find_field_line_fold(field_section);
        first_fold != std::string_view::npos) {
        unfolded_fields.emplace();
        detail::copy_unfolded_field_lines(
            field_section, first_fold, detail::field_line_folding_policy::http, *unfolded_fields);
        field_section = *unfolded_fields;
    }
    auto parsed_head = detail::parse_http1_client_response_head_fields(
        status_line, field_section, exchange_state_);
    if ((parsed_head.index() != 0)) {
        return fail(std::get<1>(parsed_head));
    }
    const auto& parsed_value = std::get<0>(parsed_head);

    auto planning = detail::plan_http1_client_response(
        exchange_state_, parsed_value, request_content_phase_, interim_faulty_framing_);
    if ((planning.index() != 0)) {
        return fail(std::get<1>(planning));
    }
    auto plan = std::move(std::get<0>(planning));

    // No owning response head is observable until the entire head and framing
    // plan have validated. Field views borrow the input or unfolded_head until
    // they are copied below; neither failure nor allocation commits the exchange.
    auto head = detail::http_client_response_head_access::make(
        parsed_value.received_status_, parsed_value.protocol_version_);
    detail::http_client_response_head_access::content_length(head, parsed_value.content_length_.value());
    const auto parsed_headers = parsed_value.headers_.view();
    if (!parsed_headers.empty()) {
        auto& headers = detail::http_client_response_head_access::headers(head);
        headers.reserve(parsed_headers.size());
        for (const auto& header_value : parsed_headers) {
            headers.emplace_back(header_value.name(), header_value.value());
        }
    }

    auto result_value = detail::http1_client_response_parse_result_access::parsed(
        std::move(head), std::move(plan), header_bytes);
    if (parsed_value.status_code_.is_informational() &&
        parsed_value.protocol_version_ == http_protocol_version::http10 &&
        parsed_value.saw_transfer_encoding_) {
        interim_faulty_framing_ = true;
    }
    if (parsed_value.status_code_ == http_status::continue_value) {
        request_content_phase_ = receive_continue(request_content_phase_);
    }
    if (parsed_value.status_code_ == http_status::switching_protocols || parsed_value.status_code_.is_final()) {
        phase_ = phase_type::complete;
    }
    return result_value;
}

void http1_client_response_parser::release_request_content() noexcept {
    if (phase_ == phase_type::await_response &&
        request_content_phase_ == detail::http1_client_request_content_phase::awaiting_continue) {
        request_content_phase_ =
            detail::http1_client_request_content_phase::content_released_awaiting_continue;
    }
}

http1_client_request_content_completion_status
http1_client_response_parser::complete_request_content() noexcept {
    if (phase_ != phase_type::await_response) {
        return http1_client_request_content_completion_status::exchange_terminal;
    }
    switch (request_content_phase_) {
        case detail::http1_client_request_content_phase::content_pending:
            request_content_phase_ = detail::http1_client_request_content_phase::content_complete;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::awaiting_continue:
        case detail::http1_client_request_content_phase::content_released_awaiting_continue:
            request_content_phase_ =
                detail::http1_client_request_content_phase::content_complete_awaiting_continue;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::continue_received:
            request_content_phase_ =
                detail::http1_client_request_content_phase::continue_received_content_complete;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::content_complete:
        case detail::http1_client_request_content_phase::content_complete_awaiting_continue:
        case detail::http1_client_request_content_phase::continue_received_content_complete:
            return http1_client_request_content_completion_status::already_complete;
    }
    return http1_client_request_content_completion_status::already_complete;
}

}  // namespace ruvia

namespace ruvia::detail {

namespace {

[[nodiscard]] bool request_offers_protocol(
    const http1_client_exchange_state& exchange_state, const http_upgrade_protocol& selected) noexcept {
    const auto offered_protocols =
        http1_client_exchange_state_access::offered_upgrade_protocols(exchange_state);
    if (offered_protocols.empty()) {
        return false;
    }
    bool offered = false;
    http_upgrade_protocols protocols;
    // The writer already validated this offer. Forwarded Upgrade fields may
    // retain empty members (RFC 9110 sections 2.2 and 5.6.1); selecting a
    // response protocol must not reapply generation grammar to those bytes.
    if (protocols.parse_field(offered_protocols, http_field_list_role::recipient,
            [&selected, &offered](const http_upgrade_protocol& candidate_value) noexcept {
                offered = offered || http_upgrade_protocol_equals(candidate_value, selected);
                return true;
            }) != http_field_list_parse_status::ok) {
        return false;
    }
    return protocols.has_protocol() && offered;
}

}  // namespace

http1_client_status_line_parse_result_type parse_http1_client_status_line(std::string_view status_line) noexcept {
    const auto separator = status_line.find(' ');
    if (separator == std::string_view::npos) {
        return http1_client_response_parse_error::invalid_status_line;
    }

    const auto version = parse_http1_version(status_line.substr(0, separator));
    if (version == http1_version_parse_result::invalid || version == http1_version_parse_result::unsupported) {
        return http1_client_response_parse_error::unsupported_http_version;
    }

    // status-line = HTTP-version SP status-code SP [ reason-phrase ]. The
    // separator after the three digits is mandatory even for an empty phrase.
    if (status_line.size() < separator + 5 || status_line[separator + 4] != ' ') {
        return http1_client_response_parse_error::invalid_status_code;
    }

    // RFC 9110 section 15: a received 000..099 or 600..999 is processed as a
    // 5xx response.
    const auto parsed_status = http_received_status::try_parse(status_line.substr(separator + 1, 3));
    if (!parsed_status) {
        return http1_client_response_parse_error::invalid_status_code;
    }

    // Unlike a field value, reason-phrase can legitimately begin or end with
    // SP/HTAB. Validate bytes directly instead of applying OWS trimming rules.
    if (!is_valid_http_field_value_bytes(status_line.substr(separator + 5))) {
        return http1_client_response_parse_error::invalid_reason_phrase;
    }

    return http1_client_parsed_status_line{.received_status_ = *parsed_status,
        .protocol_version_ = version == http1_version_parse_result::http10 ? http_protocol_version::http10 : http_protocol_version::http11};
}

http1_client_parsed_response_head::http1_client_parsed_response_head(
    const http1_client_parsed_status_line& status_line) noexcept
    : received_status_(status_line.received_status_),
      status_code_(status_line.received_status_.status()),
      protocol_version_(status_line.protocol_version_) {}

http1_client_response_head_parse_result_type parse_http1_client_response_head_fields(
    const http1_client_parsed_status_line& status_line, std::string_view field_section,
    const http1_client_exchange_state& exchange_state) {
    http1_client_parsed_response_head output(status_line);

    const auto content_semantics = classify_http_response_content_semantics(
        http1_client_exchange_state_access::method(exchange_state), output.status_code_);
    // RFC 9112 section 6.3 rule 1 ends 1xx, 204, 304 and HEAD responses at the
    // header section whatever framing fields they carry, and rule 1 precedes
    // the rule 5 Content-Length check. A recipient therefore ignores their
    // framing fields, even invalid ones, rather than rejecting a message it can
    // still delimit exactly; HEAD and 304 Content-Length remains a retained
    // field describing the selected representation.
    const bool framing_fields_apply = content_semantics == http_response_content_semantics::with_content;
    bool parse_length_metadata = content_semantics == http_response_content_semantics::without_content &&
                                 output.status_code_ != http_status::no_content &&
                                 (http1_client_exchange_state_access::method(exchange_state) == http_known_method::head ||
                                     output.status_code_ == http_status::not_modified);

    std::size_t cursor_value = 0;
    while (cursor_value < field_section.size()) {
        // The field-count rule of http_field_section_budget.
        if (output.headers_.size() == max_http_header_fields) {
            return http1_client_response_parse_error::header_too_large;
        }
        const auto field = scan_http1_field_line(field_section, cursor_value);
        if (!field) {
            return http1_client_response_parse_error::invalid_header;
        }
        const auto name = field->name();
        const auto value = field->value();
        const auto kind = classify_http_field(name);
        output.headers_.append(http_header_view{name, value});

        switch (kind) {
            case http_field_kind::content_length:
                // RFC 9112 section 6.3 rule 1: 1xx (including 101), 204, 304,
                // and HEAD responses end at the first empty line regardless of
                // framing fields; successful CONNECT becomes a tunnel.
                if (framing_fields_apply || parse_length_metadata) {
                    if (const auto error = parse_http1_content_length_field<http1_client_response_parse_error>(value, output.content_length_)) {
                        if (framing_fields_apply) {
                            return *error;
                        }
                        // 无正文优先级不变；不一致的表示元数据只是不再提供已知长度。
                        output.content_length_ = {};
                        parse_length_metadata = false;
                    }
                }
                break;
            case http_field_kind::connection:
                if (output.connection_options_.parse_field(value, http_connection_field_role::response_recipient) !=
                    http_field_list_parse_status::ok) {
                    return http1_client_response_parse_error::invalid_connection;
                }
                break;
            case http_field_kind::transfer_encoding:
                output.saw_transfer_encoding_ = true;
                // Only a response with content is framed by Transfer-Encoding;
                // successful CONNECT is ignored by client-side rule.
                if (framing_fields_apply && output.protocol_version_ == http_protocol_version::http11) {
                    if (const auto error = parse_http1_transfer_encoding_field<http1_client_response_parse_error>(value, output.transfer_encoding_)) {
                        return *error;
                    }
                }
                break;
            case http_field_kind::upgrade:
                if (output.upgrade_protocols_.parse_field(value, http_field_list_role::recipient,
                        [&exchange_state, &output,
                            check_offered = content_semantics ==
                                            http_response_content_semantics::protocol_switch](
                            const http_upgrade_protocol& selected) noexcept {
                            if (check_offered && output.all_upgrade_protocols_offered_ &&
                                !request_offers_protocol(exchange_state, selected)) {
                                output.all_upgrade_protocols_offered_ = false;
                            }
                            return true;
                        }) != http_field_list_parse_status::ok) {
                    return http1_client_response_parse_error::invalid_upgrade;
                }
                break;
            default:
                break;
        }
    }

    if (output.transfer_encoding_.unsupported()) {
        return http1_client_response_parse_error::unsupported_transfer_encoding;
    }
    // RFC 9112 section 6.1: an HTTP/1.0 message carrying Transfer-Encoding is
    // treated as if its framing were faulty and the connection closes after it.
    // The transfer codings stay unparsed; the plan ignores Transfer-Encoding and
    // falls back to Content-Length or close delimiting.
    return output;
}

}  // namespace ruvia::detail

namespace ruvia {

std::optional<std::string_view> http_client_response_head::header(
    std::string_view name) const& noexcept {
    return detail::find_last_http_header<http_header>(headers_, name);
}

}  // namespace ruvia

// Whether a parsed response head lets the exchange continue: may this 101 switch
// protocols given what the request offered, does the body have a length, and
// does the connection survive the response.

namespace ruvia::detail {

namespace {

[[nodiscard]] bool request_allows_protocol_switch(const http1_client_exchange_state& exchange_state,
    const http1_client_parsed_response_head& response,
    http1_client_request_content_phase request_content_phase,
    bool interim_faulty_framing) noexcept {
    const bool request_content_allows_switch =
        request_content_phase == http1_client_request_content_phase::content_complete ||
        request_content_phase == http1_client_request_content_phase::content_complete_awaiting_continue ||
        request_content_phase == http1_client_request_content_phase::continue_received_content_complete;
    if (interim_faulty_framing ||
        http1_client_exchange_state_access::close_policy(exchange_state) ==
            http1_close_policy::close_after_response ||
        !request_content_allows_switch) {
        return false;
    }
    if (detail::http1_client_exchange_state_access::offered_upgrade_protocols(exchange_state).empty() ||
        response.connection_options_.close() || !response.connection_options_.upgrade() ||
        !response.upgrade_protocols_.has_protocol()) {
        return false;
    }
    return response.all_upgrade_protocols_offered_;
}

[[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal(
    http1_client_request_content_phase phase, http_status_code status_code,
    bool response_will_close) noexcept {
    if (response_will_close && (phase == http1_client_request_content_phase::awaiting_continue ||
                                   phase == http1_client_request_content_phase::content_pending ||
                                   phase == http1_client_request_content_phase::content_released_awaiting_continue ||
                                   phase == http1_client_request_content_phase::continue_received)) {
        return http_client_request_content_signal::exchange_complete;
    }
    if (status_code == http_status::continue_value) {
        return phase == http1_client_request_content_phase::awaiting_continue
                   ? std::optional<http_client_request_content_signal>(
                         http_client_request_content_signal::continue_value)
                   : std::nullopt;
    }
    if (status_code.is_final() && phase == http1_client_request_content_phase::awaiting_continue) {
        // A final response cancels content only while Expect still gates it.
        // After 100 Continue or a finite wait releases the writer, RFC 9110
        // section 7.5 says to keep sending unless the server explicitly stops
        // it. Closing responses provide that signal and are handled above.
        return http_client_request_content_signal::exchange_complete;
    }
    return std::nullopt;
}

[[nodiscard]] http1_close_policy response_persistence(
    const http1_client_parsed_response_head& response) noexcept {
    // RFC 9112 section 6.1: an HTTP/1.0 message carrying Transfer-Encoding
    // has faulty framing and the connection closes after it, even when the
    // response ends at its header section and the field is otherwise ignored.
    if (response.connection_options_.close() ||
        (response.protocol_version_ == http_protocol_version::http10 && response.saw_transfer_encoding_)) {
        return http1_close_policy::close_after_response;
    }
    if (response.protocol_version_ == http_protocol_version::http11 ||
        response.connection_options_.keep_alive()) {
        return http1_close_policy::allow_reuse;
    }
    return http1_close_policy::close_after_response;
}

[[nodiscard]] http1_close_policy final_response_persistence(
    const http1_client_exchange_state& exchange_state,
    const http1_client_parsed_response_head& response,
    http1_client_request_content_phase request_content_phase,
    bool interim_faulty_framing) noexcept {
    // A final response while Expect still gates the content cancels content
    // whose framing the request head already committed. The server may keep
    // reading it (RFC 9110 section 10.1.1), so the next request on this
    // connection would be consumed as that content.
    // A rejected CONNECT is not a reusable HTTP exchange: RFC 9931 section 8
    // requires the proxy to close it before interpreting any following request.
    if (interim_faulty_framing ||
        http1_client_exchange_state_access::method(exchange_state) == http_known_method::connect ||
        http1_client_exchange_state_access::close_policy(exchange_state) ==
            http1_close_policy::close_after_response ||
        request_content_phase == http1_client_request_content_phase::awaiting_continue) {
        return http1_close_policy::close_after_response;
    }
    return response_persistence(response);
}

}  // namespace

struct http1_client_response_plan_access final {
    using request_content_signal_type = std::optional<http_client_request_content_signal>;

    [[nodiscard]] static http1_client_response_plan informational(
        http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_informational_response(persistence)),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan without_content(
        http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_response_without_content(persistence)),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan known_length(std::size_t content_length,
        http1_close_policy persistence, request_content_signal_type request_content_signal,
        bool discards_content) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(
                http1_client_known_length_response(content_length, persistence)),
            request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan chunked(http_transfer_codings transfer_codings,
        http1_close_policy persistence, request_content_signal_type request_content_signal,
        bool discards_content) {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_chunked_response(
                                              std::move(transfer_codings), persistence)),
            request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan close_delimited(
        http_transfer_codings transfer_codings, request_content_signal_type request_content_signal,
        bool discards_content) {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_close_delimited_response(
                std::move(transfer_codings))),
            request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan connect_tunnel() noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_connect_tunnel()), std::nullopt);
    }

    [[nodiscard]] static http1_client_response_plan protocol_upgrade() noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_protocol_upgrade()), std::nullopt);
    }
};

http1_client_response_planning_result_type plan_http1_client_response(
    const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response,
    http1_client_request_content_phase request_content_phase, bool interim_faulty_framing) {
    const auto content_semantics = classify_http_response_content_semantics(
        http1_client_exchange_state_access::method(exchange_state), response.status_code_);

    if (content_semantics == http_response_content_semantics::protocol_switch) {
        if (response.protocol_version_ != http_protocol_version::http11 ||
            !request_allows_protocol_switch(
                exchange_state, response, request_content_phase, interim_faulty_framing)) {
            return http1_client_response_parse_error::invalid_protocol_switch;
        }
        return http1_client_response_plan_access::protocol_upgrade();
    }
    if (content_semantics == http_response_content_semantics::informational) {
        // A 1xx never ends the exchange or cancels request content, so it cannot
        // close the connection either; the final response alone decides that
        // (RFC 9110 Section 15.2).
        return http1_client_response_plan_access::informational(http1_close_policy::allow_reuse,
            request_content_signal(request_content_phase, response.status_code_, false));
    }
    if (content_semantics == http_response_content_semantics::connect_tunnel) {
        return http1_client_response_plan_access::connect_tunnel();
    }

    // RFC 9110 section 15.3.6 forbids only generating 205 content. Its framing
    // is still authoritative, so delimited content is consumed and discarded
    // and the connection stays reusable.
    const bool discards_content = response.status_code_ == http_status::reset_content &&
                                  content_semantics == http_response_content_semantics::with_content;
    const auto content_length = response.content_length_.value();

    const auto persistence = final_response_persistence(
        exchange_state, response, request_content_phase, interim_faulty_framing);
    const auto persistent_content_signal = request_content_signal(request_content_phase,
        response.status_code_, persistence == http1_close_policy::close_after_response);
    if (content_semantics == http_response_content_semantics::without_content) {
        return http1_client_response_plan_access::without_content(persistence, persistent_content_signal);
    }

    const auto& transfer_encoding = response.transfer_encoding_.value();
    // RFC 9112 section 6.1: HTTP/1.0 Transfer-Encoding is faulty framing, so the
    // codings are ignored and the message falls back to Content-Length or close
    // delimiting while the connection closes after it.
    const bool http10_faulty_framing = response.protocol_version_ == http_protocol_version::http10 &&
                                       response.saw_transfer_encoding_;
    if (response.saw_transfer_encoding_ && !http10_faulty_framing) {
        if (content_length.has_value()) {
            return http1_client_response_parse_error::content_length_and_transfer_encoding;
        }
        if (!transfer_encoding.has_value()) {
            return http1_client_response_parse_error::invalid_transfer_encoding;
        }
        if (const auto* final_chunked = transfer_encoding->final_chunked()) {
            return http1_client_response_plan_access::chunked(
                final_chunked->transfer_codings(), persistence, persistent_content_signal,
                discards_content);
        }
        return http1_client_response_plan_access::close_delimited(
            transfer_encoding->non_chunked()->transfer_codings(),
            request_content_signal(request_content_phase, response.status_code_, true),
            discards_content);
    }

    if (content_length.has_value()) {
        if (*content_length > (std::numeric_limits<std::size_t>::max)()) {
            return http1_client_response_parse_error::invalid_content_length;
        }
        return http1_client_response_plan_access::known_length(
            static_cast<std::size_t>(*content_length), persistence, persistent_content_signal,
            discards_content);
    }

    // RFC 9112 section 6.3: a body-allowed response with no declared
    // length is delimited by server close and cannot return to a pool.
    return http1_client_response_plan_access::close_delimited(
        http_transfer_codings{},
        request_content_signal(request_content_phase, response.status_code_, true),
        discards_content);
}

}  // namespace ruvia::detail

// Media-type syntax (RFC 9110 section 8.3.1): splitting a field value into
// type/subtype and parameters, comparing parameter values across token and
// quoted-string forms, and validating a Content-Type field. Matching a media
// type against an Accept media-range lives in http_accept_match.cpp.

namespace ruvia::detail {

struct http_media_type_parts final {
    std::string_view type_;
    std::string_view subtype_;
};

// Both values have already passed parameter syntax validation. Compare their
// decoded bytes without materializing quoted strings.
[[nodiscard]] inline bool http_media_parameter_value_equals(
    std::string_view left, std::string_view right, bool ascii_case_insensitive = false) noexcept {
    if (left.front() != '"' && right.front() != '"') {
        return ascii_case_insensitive ? http_ascii_equals_ignore_case(left, right) : left == right;
    }
    struct cursor final {
        std::string_view value_;
        http_quoted_string_cursor quoted_cursor_;
        std::size_t position_{0};

        explicit cursor(std::string_view value) noexcept
            : value_(value),
              quoted_cursor_(value) {}

        [[nodiscard]] bool next(unsigned char& out) noexcept {
            if (value_.front() == '"') {
                return quoted_cursor_.next(out);
            }
            if (position_ == value_.size()) {
                return false;
            }
            out = static_cast<unsigned char>(value_[position_++]);
            return true;
        }
    };
    cursor lhs(left);
    cursor rhs(right);
    while (true) {
        unsigned char lhs_char = 0;
        unsigned char rhs_char = 0;
        const bool has_left = lhs.next(lhs_char);
        const bool has_right = rhs.next(rhs_char);
        if (has_left != has_right) {
            return false;
        }
        if (!has_left) {
            return true;
        }
        if (ascii_case_insensitive) {
            lhs_char = http_ascii_to_lower(lhs_char);
            rhs_char = http_ascii_to_lower(rhs_char);
        }
        if (lhs_char != rhs_char) {
            return false;
        }
    }
}

// Parameters are cached on the stack. A media type or media range with more
// than max_parameters parameters is invalid, which bounds the per-member
// duplicate-name check. Views stay valid only while the parsed field is
// unchanged; parsing and matching never allocate.
struct http_media_type_parameters final {
    static constexpr std::size_t max_parameters = 64;

    struct parameter final {
        const char* name_data_;
        std::size_t name_size_;
        const char* value_data_;
        std::size_t value_size_;

        [[nodiscard]] std::string_view name() const noexcept {
            return {name_data_, name_size_};
        }

        [[nodiscard]] std::string_view value() const noexcept {
            return {value_data_, value_size_};
        }
    };
    // Only the first size_ slots are initialized and read. Plain pointer/length
    // slots avoid constructing string_views for unused slots.
    std::array<parameter, max_parameters> values_;
    std::size_t size_{0};

    template <typename visitor_type>
    [[nodiscard]] bool visit(visitor_type&& visitor) const {
        for (std::size_t i = 0; i < size_; ++i) {
            if (!visitor(values_[i].name(), values_[i].value())) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<std::string_view> find(std::string_view name) const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            if (http_ascii_equals_ignore_case(values_[i].name(), name)) {
                return values_[i].value();
            }
        }
        return std::nullopt;
    }

    // A quality output selects Accept semantics: q is parsed as a weight,
    // including an invalid weight's zero quality, rather than a media parameter.
    [[nodiscard]] bool parse(std::string_view value, int* quality = nullptr) noexcept {
        size_ = 0;
        if (quality != nullptr) {
            *quality = 1000;
        }
        return http_all_parameters(value, [&](std::string_view part) noexcept {
            if (part.empty()) {
                return true;
            }
            const auto equals = part.find('=');
            if (equals == std::string_view::npos || size_ == values_.size()) {
                return false;
            }
            const auto name = part.substr(0, equals);
            const auto parameter_value = part.substr(equals + 1);
            if (!is_valid_http_header_name(name) || parameter_value.empty() || parameter_value != http_trim_ows(parameter_value)) {
                return false;
            }
            if (find(name).has_value()) {
                return false;
            }
            if (quality != nullptr && http_ascii_equals_ignore_case(name, "q")) {
                const auto parsed = http_parse_quality_value(parameter_value);
                *quality = parsed < 0 ? 0 : parsed;
            } else if (!is_http_token_or_quoted_string(parameter_value)) {
                return false;
            }
            values_[size_++] = {name.data(), name.size(), parameter_value.data(), parameter_value.size()};
            return true;
        });
    }

    [[nodiscard]] bool contains(std::string_view name, std::string_view value,
        const http_media_type_parts& parts) const noexcept {
        const auto parameter_value = find(name);
        if (!parameter_value.has_value()) {
            return false;
        }
        // RFC 3676 section 4 defines these case-insensitive values only for text/plain.
        const bool ascii_case_insensitive = http_ascii_equals_ignore_case(name, "charset") ||
                                            ((http_ascii_equals_ignore_case(name, "format") ||
                                                 http_ascii_equals_ignore_case(name, "delsp")) &&
                                                http_ascii_equals_ignore_case(parts.type_, "text") &&
                                                http_ascii_equals_ignore_case(parts.subtype_, "plain"));
        return http_media_parameter_value_equals(*parameter_value, value, ascii_case_insensitive);
    }
};

[[nodiscard]] inline bool http_parse_media_type_parts(
    std::string_view value, bool allow_wildcard, http_media_type_parts& parts) noexcept {
    value = http_header_token_before_parameters(value);
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }

    parts.type_ = value.substr(0, slash);
    parts.subtype_ = value.substr(slash + 1);
    const bool type_wildcard = parts.type_ == "*";
    const bool subtype_wildcard = parts.subtype_ == "*";
    if ((type_wildcard || subtype_wildcard) && !allow_wildcard) {
        return false;
    }
    if (type_wildcard && !subtype_wildcard) {
        return false;
    }
    return (type_wildcard || is_valid_http_header_name(parts.type_)) &&
           (subtype_wildcard || is_valid_http_header_name(parts.subtype_));
}

template <http_temporary_owning_char_string value_type>
bool http_parse_media_type_parts(value_type&&, bool, http_media_type_parts&) = delete;

[[nodiscard]] inline bool http_parse_media_type(std::string_view value, http_media_type_parts& parts,
    http_media_type_parameters& parameters) noexcept {
    return http_parse_media_type_parts(value, false, parts) && parameters.parse(value);
}

template <http_temporary_owning_char_string value_type>
bool http_parse_media_type(value_type&&, http_media_type_parts&, http_media_type_parameters&) = delete;

[[nodiscard]] inline bool is_valid_http_content_type_field_value(std::string_view value) noexcept {
    http_media_type_parts parts;
    http_media_type_parameters parameters;
    return http_parse_media_type(value, parts, parameters);
}

template <http_temporary_owning_char_string value_type>
bool is_valid_http_content_type_field_value(value_type&&) = delete;

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

struct http_field_definition final {
    std::string_view name_;
    http_field_kind kind_;
    std::uint32_t response_bit_;
};

// One spelling authority for both directions. Length-indexed ranges keep
// classification bounded to the names of the same size without allocation.
constexpr http_field_definition http_field_definitions[] = {
    {"te", http_field_kind::te, 0},
    {"date", http_field_kind::other, response_header_date},
    {"etag", http_field_kind::other, response_header_etag},
    {"host", http_field_kind::host, 0},
    {"vary", http_field_kind::other, response_header_vary},
    {"allow", http_field_kind::other, response_header_allow},
    {"range", http_field_kind::range, 0},
    {"accept", http_field_kind::accept, 0},
    {"cookie", http_field_kind::cookie, 0},
    {"expect", http_field_kind::expect, 0},
    {"origin", http_field_kind::origin, 0},
    {"server", http_field_kind::other, response_header_server},
    {"trailer", http_field_kind::trailer, response_header_trailer},
    {"upgrade", http_field_kind::upgrade, response_header_upgrade},
    {"if-match", http_field_kind::if_match, 0},
    {"if-range", http_field_kind::if_range, 0},
    {"location", http_field_kind::other, response_header_location},
    {"forwarded", http_field_kind::forwarded, 0},
    {"connection", http_field_kind::connection, response_header_connection},
    {"keep-alive", http_field_kind::other, response_header_keep_alive},
    {"set-cookie", http_field_kind::other, response_header_set_cookie},
    {"user-agent", http_field_kind::user_agent, 0},
    {"content-type", http_field_kind::content_type, response_header_content_type},
    {"accept-ranges", http_field_kind::other, response_header_accept_ranges},
    {"authorization", http_field_kind::authorization, 0},
    {"cache-control", http_field_kind::other, response_header_cache_control},
    {"content-range", http_field_kind::other, response_header_content_range},
    {"if-none-match", http_field_kind::if_none_match, 0},
    {"last-modified", http_field_kind::other, response_header_last_modified},
    {"content-length", http_field_kind::content_length, response_header_content_length},
    {"accept-encoding", http_field_kind::accept_encoding, 0},
    {"x-forwarded-for", http_field_kind::x_forwarded_for, 0},
    {"content-encoding", http_field_kind::content_encoding, response_header_content_encoding},
    {"proxy-connection", http_field_kind::other, response_header_proxy_connection},
    {"if-modified-since", http_field_kind::if_modified_since, 0},
    {"sec-websocket-key", http_field_kind::sec_websocket_key, 0},
    {"transfer-encoding", http_field_kind::transfer_encoding, response_header_transfer_encoding},
    {"x-forwarded-proto", http_field_kind::x_forwarded_proto, 0},
    {"if-unmodified-since", http_field_kind::if_unmodified_since, 0},
    {"proxy-authorization", http_field_kind::proxy_authorization, 0},
    {"sec-websocket-version", http_field_kind::sec_websocket_version, 0},
    {"access-control-max-age", http_field_kind::other, response_header_access_control_max_age},
    {"sec-websocket-protocol", http_field_kind::sec_websocket_protocol, 0},
    {"sec-websocket-extensions", http_field_kind::sec_websocket_extensions, 0},
    {"access-control-allow-origin", http_field_kind::other, response_header_access_control_allow_origin},
    {"access-control-allow-headers", http_field_kind::other, response_header_access_control_allow_headers},
    {"access-control-allow-methods", http_field_kind::other, response_header_access_control_allow_methods},
    {"access-control-expose-headers", http_field_kind::other, response_header_access_control_expose_headers},
    {"access-control-request-method", http_field_kind::access_control_request_method, 0},
    {"access-control-request-headers", http_field_kind::access_control_request_headers, 0},
    {"access-control-allow-credentials", http_field_kind::other, response_header_access_control_allow_credentials},
};

static_assert([] {
    for (std::size_t i = 1; i < std::size(http_field_definitions); ++i) {
        if (http_field_definitions[i - 1].name_.size() > http_field_definitions[i].name_.size()) {
            return false;
        }
    }
    return true;
}());

constexpr auto http_field_length_offsets = [] {
    std::array<std::size_t, http_field_definitions[std::size(http_field_definitions) - 1].name_.size() + 2> offsets{};
    std::size_t index = 0;
    for (std::size_t length = 0; length < offsets.size(); ++length) {
        while (index < std::size(http_field_definitions) && http_field_definitions[index].name_.size() < length) {
            ++index;
        }
        offsets[length] = index;
    }
    return offsets;
}();

constexpr auto response_field_names = [] {
    std::array<std::string_view, std::bit_width(response_header_proxy_connection)> names{};
    for (const auto& field : http_field_definitions) {
        if (field.response_bit_ != 0) {
            names[std::countr_zero(field.response_bit_)] = field.name_;
        }
    }
    return names;
}();

}  // namespace

http_field_classification classify_http_field_name(std::string_view name) noexcept {
    if (name.empty() || name.size() >= http_field_length_offsets.size() - 1) {
        return {};
    }
    const auto first = http_ascii_to_lower(static_cast<unsigned char>(name.front()));
    const auto end = http_field_length_offsets[name.size() + 1];
    for (auto index = http_field_length_offsets[name.size()]; index < end; ++index) {
        const auto& field = http_field_definitions[index];
        if (first == static_cast<unsigned char>(field.name_.front()) && http_ascii_equals_ignore_case(name, field.name_)) {
            return {field.kind_, field.response_bit_};
        }
    }
    return {};
}

http_field_kind classify_http_field(std::string_view name) noexcept {
    return classify_http_field_name(name).kind_;
}

std::uint32_t classify_response_header_name(std::string_view name) noexcept {
    return classify_http_field_name(name).response_bit_;
}

std::string_view response_known_header_lowercase_name(std::uint32_t bit) noexcept {
    if (!std::has_single_bit(bit)) {
        return {};
    }
    const auto index = static_cast<std::size_t>(std::countr_zero(bit));
    return index < response_field_names.size() ? response_field_names[index] : std::string_view{};
}

}  // namespace ruvia::detail

// The method vocabulary: recognising the standard tokens, spelling them back,
// validating an unknown one, and the two properties -- safe and idempotent --
// that decide what a recipient may do with a request (RFC 9110 section 9.2).

namespace ruvia {

http_known_method classify_http_method(std::string_view method) noexcept {
    switch (method.size()) {
        case 3:
            if (method == "GET") {
                return http_known_method::get;
            }
            if (method == "PUT") {
                return http_known_method::put;
            }
            break;
        case 4:
            if (method == "POST") {
                return http_known_method::post;
            }
            if (method == "HEAD") {
                return http_known_method::head;
            }
            break;
        case 5:
            if (method == "PATCH") {
                return http_known_method::patch;
            }
            break;
        case 6:
            if (method == "DELETE") {
                return http_known_method::delete_value;
            }
            break;
        case 7:
            if (method == "OPTIONS") {
                return http_known_method::options;
            }
            if (method == "CONNECT") {
                return http_known_method::connect;
            }
            break;
        default:
            break;
    }
    return http_known_method::unknown;
}

std::string_view known_http_method_token(http_known_method method) noexcept {
    switch (method) {
        case http_known_method::get:
            return "GET";
        case http_known_method::post:
            return "POST";
        case http_known_method::put:
            return "PUT";
        case http_known_method::delete_value:
            return "DELETE";
        case http_known_method::patch:
            return "PATCH";
        case http_known_method::head:
            return "HEAD";
        case http_known_method::options:
            return "OPTIONS";
        case http_known_method::connect:
            return "CONNECT";
        case http_known_method::unknown:
        default:
            return {};
    }
}

bool is_valid_http_method_token(std::string_view method) noexcept {
    return is_valid_http_header_name(method);
}

bool is_http_method_safe(std::string_view method) noexcept {
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE";
}

bool is_http_method_idempotent(std::string_view method) noexcept {
    return is_http_method_safe(method) || method == "PUT" || method == "DELETE";
}

}  // namespace ruvia

namespace ruvia {

bool is_http_hop_by_hop_field_name(std::string_view field_name) noexcept {
    switch (field_name.size()) {
        case 2U:
            return http_ascii_equals_ignore_case(field_name, "TE");
        case 7U:
            return http_ascii_equals_ignore_case(field_name, "Trailer") ||
                   http_ascii_equals_ignore_case(field_name, "Upgrade");
        case 10U:
            return http_ascii_equals_ignore_case(field_name, "Connection") ||
                   http_ascii_equals_ignore_case(field_name, "Keep-Alive");
        case 16U:
            return http_ascii_equals_ignore_case(field_name, "Proxy-Connection");
        case 17U:
            return http_ascii_equals_ignore_case(field_name, "Transfer-Encoding");
        case 18U:
            return http_ascii_equals_ignore_case(field_name, "Proxy-Authenticate");
        case 19U:
            return http_ascii_equals_ignore_case(field_name, "Proxy-Authorization");
        default:
            return false;
    }
}

bool is_http_standard_connection_option(std::string_view option) noexcept {
    return (option.size() == 5U && http_ascii_equals_ignore_case(option, "close")) ||
           is_http_hop_by_hop_field_name(option);
}

void http_connection_nominated_fields::update(std::string_view connection_field_value) {
    detail::http_visit_comma_separated_tokens(connection_field_value, [&](std::string_view token) {
        if (!is_valid_http_header_name(token)) {
            return true;
        }
        if (http_ascii_equals_ignore_case(token, "close")) {
            has_close_ = true;
        } else if (http_ascii_equals_ignore_case(token, "keep-alive")) {
            has_keep_alive_ = true;
        } else if (http_ascii_equals_ignore_case(token, "Upgrade")) {
            has_upgrade_ = true;
        } else if (http_ascii_equals_ignore_case(token, "TE")) {
            has_te_ = true;
        }
        if (contains(token)) {
            return true;
        }
        std::string lower(token);
        for (auto& ch : lower) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        tokens_.push_back(std::move(lower));
        return true;
    });
}

void http_connection_nominated_fields::update_header_range(detail::http_header_range_view headers) {
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Connection")) {
            update(field.value());
        }
    }
}

bool http_connection_nominated_fields::contains(std::string_view field_name) const noexcept {
    return std::ranges::any_of(tokens_, [field_name](std::string_view token) noexcept {
        return http_ascii_equals_ignore_case(token, field_name);
    });
}

std::vector<std::string> http_connection_nominated_fields::non_standard_options() const {
    std::vector<std::string> result;
    for (const auto& token : tokens_) {
        if (!is_http_standard_connection_option(token)) {
            result.emplace_back(token);
        }
    }
    return result;
}

std::string http_connection_nominated_fields::rebuild_connection_field(
    bool retain_upgrade, bool include_non_standard) const {
    std::string output;
    if (retain_upgrade && has_upgrade_) {
        output.append("Upgrade");
    }
    if (include_non_standard) {
        for (const auto& token : tokens_) {
            if (is_http_standard_connection_option(token)) {
                continue;
            }
            if (!output.empty()) {
                output.append(", ");
            }
            output.append(token);
        }
    }
    return output;
}

http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::string_view connection_field_value) {
    http_connection_nominated_fields nominated;
    nominated.update(connection_field_value);
    return nominated;
}
bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated, bool retain_upgrade) noexcept {
    if (retain_upgrade && nominated.has_upgrade() &&
        http_ascii_equals_ignore_case(field_name, "Upgrade")) {
        return false;
    }
    return is_http_hop_by_hop_field_name(field_name) || nominated.contains(field_name);
}

bool is_http_hop_by_hop_trailer_field(
    std::string_view field_name, const http_connection_nominated_fields& nominated) noexcept {
    return is_http_hop_by_hop_field_name(field_name) || nominated.contains(field_name);
}

bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept {
    return detail::is_forbidden_http_request_trailer_name(field_name);
}

bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept {
    return detail::is_forbidden_response_trailer_name(field_name);
}
}  // namespace ruvia

namespace ruvia {

http_conditional_headers get_http_conditional_headers(const http_request& request) noexcept {
    return http_conditional_headers{
        detail::http_request_access::has_repeated_known_header(
            request, detail::http_field_kind::if_unmodified_since)
            ? std::string_view{}
            : detail::http_request_access::known_header(request, detail::http_field_kind::if_unmodified_since),
        detail::http_request_access::has_repeated_known_header(
            request, detail::http_field_kind::if_modified_since)
            ? std::string_view{}
            : detail::http_request_access::known_header(request, detail::http_field_kind::if_modified_since),
        detail::http_request_access::known_header(request, detail::http_field_kind::range),
        detail::http_request_access::known_header(request, detail::http_field_kind::if_range),
        detail::http_request_access::has_known_header(request, detail::http_field_kind::if_range)};
}

void http_etag_field_condition::update(
    std::string_view value, std::string_view expected, bool strong) noexcept {
    const bool already_present = present_;
    present_ = true;
    const auto trimmed = http_trim_ows(value);
    if (trimmed == "*") {
        wildcard_ = true;
        if (already_present) {
            valid_ = false;
        }
        return;
    }
    if (wildcard_) {
        valid_ = false;
    }
    const auto result_value = detail::http_parse_etag_list_matches(value, expected, strong);
    valid_ = valid_ && result_value.valid_;
    matched_ = matched_ || result_value.matched_;
}

http_etag_preconditions get_http_etag_preconditions(
    const http_request& request, std::string_view etag) noexcept {
    http_etag_preconditions result;
    // Both conditions are RFC list fields. Multiple field lines are equivalent
    // to comma-joining their values (RFC 9110 §5.3), but the request keeps
    // zero-copy views into separate wire lines. Fold them line by line and
    // retain whole-list validity without allocating a joined string.
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::if_match, [&result, etag](std::string_view value) noexcept {
            result.if_match_.update(value, etag, true);
            return true;
        });
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::if_none_match, [&result, etag](std::string_view value) noexcept {
            result.if_none_match_.update(value, etag, false);
            return true;
        });
    return result;
}

bool http_date_not_modified(std::string_view header_value, std::time_t modified_seconds) noexcept {
    const auto date = parse_http_date(header_value);
    return date.has_value() && modified_seconds <= *date;
}

bool http_date_unmodified(std::string_view header_value, std::time_t modified_seconds) noexcept {
    const auto date = parse_http_date(header_value);
    return !date.has_value() || modified_seconds <= *date;
}

bool http_if_range_allows(std::string_view header_value, std::string_view etag,
    std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept {
    if (header_value.empty()) {
        return false;
    }
    const auto value = http_trim_ows(header_value);
    if (!value.empty() && (value.front() == '"' || value.starts_with("W/"))) {
        return detail::http_strong_etag_equals(value, etag);
    }
    if (!date_validator_strong || !modified_seconds) {
        return false;
    }
    const auto date = parse_http_date(value);
    return date.has_value() && *modified_seconds == *date;
}

bool is_valid_http_entity_tag(std::string_view value) noexcept {
    if (value.starts_with("W/")) {
        value.remove_prefix(2);
    }
    if (value.size() < 2U || value.front() != '"' || value.back() != '"') {
        return false;
    }
    for (std::size_t i = 1U; i + 1U < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte == '"' || (byte != 0x21U && !(byte >= 0x23U && byte <= 0x7eU) && byte < 0x80U)) {
            return false;
        }
    }
    return true;
}

bool is_weak_http_entity_tag(std::string_view value) noexcept {
    return value.starts_with("W/") && is_valid_http_entity_tag(value);
}

bool is_strong_http_entity_tag(std::string_view value) noexcept {
    return !value.starts_with("W/") && is_valid_http_entity_tag(value);
}

bool http_strong_etag_equals(std::string_view left, std::string_view right) noexcept {
    return is_strong_http_entity_tag(left) && is_strong_http_entity_tag(right) && left == right;
}

bool http_weak_etag_equals(std::string_view left, std::string_view right) noexcept {
    return is_valid_http_entity_tag(left) && is_valid_http_entity_tag(right) &&
           detail::http_weak_etag_equals(left, right);
}

std::string weaken_http_etag(std::string_view etag) {
    etag = http_trim_ows(etag);
    if (is_strong_http_entity_tag(etag)) {
        std::string weakened;
        weakened.reserve(2U + etag.size());
        weakened.append("W/");
        weakened.append(etag);
        return weakened;
    }
    return std::string(etag);
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

enum class http_accept_token_match : int {
    none = -1,
    wildcard = 0,
    prefix = 1,
    exact = 2,
};

// RFC 4647 section 3.3.1 basic filtering: a language range matches a tag that
// equals it or extends it at a subtag boundary, so "en" matches "en-US" but
// never "english". Only Accept-Language uses this; charsets match exactly or by
// wildcard.
[[nodiscard]] http_accept_token_match http_accept_token_matches(
    std::string_view range, std::string_view offered, bool prefix_matching) noexcept {
    if (range == "*") {
        return http_accept_token_match::wildcard;
    }
    if (http_ascii_equals_ignore_case(range, offered)) {
        return http_accept_token_match::exact;
    }
    if (prefix_matching && offered.size() > range.size() && offered[range.size()] == '-' &&
        http_ascii_equals_ignore_case(range, offered.substr(0, range.size()))) {
        return http_accept_token_match::prefix;
    }
    return http_accept_token_match::none;
}

void http_accumulate_token_acceptance(std::string_view accept, std::string_view offered,
    bool prefix_matching, int& best_specificity, int& best_quality) noexcept {
    http_visit_comma_separated_quoted(accept,
        [offered, prefix_matching, &best_specificity, &best_quality](std::string_view item) noexcept {
            const auto range = http_header_token_before_parameters(item);
            const auto match = http_accept_token_matches(range, offered, prefix_matching);
            if (match == http_accept_token_match::none) {
                return true;
            }
            const auto specificity = prefix_matching && match != http_accept_token_match::wildcard
                                         ? static_cast<int>(std::min(range.size(),
                                               static_cast<std::size_t>((std::numeric_limits<int>::max)())))
                                         : static_cast<int>(match);
            const auto quality = http_weight_parameter(item);
            if (specificity > best_specificity ||
                (specificity == best_specificity && quality > best_quality)) {
                best_specificity = specificity;
                best_quality = quality;
            }
            return true;
        });
}

[[nodiscard]] int http_media_range_match_specificity(std::string_view range,
    const http_media_type_parts& offered_parts, const http_media_type_parameters& offered_parameters,
    std::size_t& parameter_count, int& quality) noexcept {
    http_media_type_parts range_parts;
    if (!http_parse_media_type_parts(range, true, range_parts)) {
        return -1;
    }
    if (range_parts.type_ != "*" && !http_ascii_equals_ignore_case(range_parts.type_, offered_parts.type_)) {
        return -1;
    }
    if (range_parts.subtype_ != "*" && !http_ascii_equals_ignore_case(range_parts.subtype_, offered_parts.subtype_)) {
        return -1;
    }
    http_media_type_parameters parameters;
    if (!parameters.parse(range, &quality)) {
        return -1;
    }
    parameter_count = 0;
    if (!parameters.visit([&](std::string_view name, std::string_view value) noexcept {
            if (http_ascii_equals_ignore_case(name, "q")) {
                return true;
            }
            if (!offered_parameters.contains(name, value, offered_parts)) {
                return false;
            }
            ++parameter_count;
            return true;
        })) {
        return -1;
    }
    return range_parts.type_ == "*" ? 0 : (range_parts.subtype_ == "*" ? 1 : 2);
}

void http_accumulate_media_type_acceptance(std::string_view accept, std::string_view offered,
    int& best_specificity, std::size_t& best_parameter_count, int& best_quality) noexcept {
    http_media_type_parts offered_parts;
    http_media_type_parameters offered_parameters;
    if (!http_parse_media_type(offered, offered_parts, offered_parameters)) {
        return;
    }
    http_visit_comma_separated_quoted(accept,
        [offered_parts, &offered_parameters, &best_specificity, &best_parameter_count, &best_quality](std::string_view item) noexcept {
            int quality = 1000;
            std::size_t parameter_count = 0;
            const auto specificity = http_media_range_match_specificity(item, offered_parts, offered_parameters, parameter_count, quality);
            if (specificity >= 0) {
                if (specificity > best_specificity ||
                    (specificity == best_specificity &&
                        (parameter_count > best_parameter_count ||
                            (parameter_count == best_parameter_count && quality > best_quality)))) {
                    best_specificity = specificity;
                    best_parameter_count = parameter_count;
                    best_quality = quality;
                }
            }
            return true;
        });
}

}  // namespace
}  // namespace ruvia::detail
namespace ruvia {

std::string_view http_media_type_only(borrowed_text value) noexcept {
    return detail::http_header_token_before_parameters(value.view());
}

bool is_valid_http_content_type_field_value(std::string_view value) noexcept {
    return detail::is_valid_http_content_type_field_value(value);
}

void http_accept_match::update_media_type(std::string_view field, std::string_view offered) noexcept {
    detail::http_accumulate_media_type_acceptance(field, offered, specificity_, parameter_specificity_, quality_);
}

void http_accept_match::update_token(std::string_view field, std::string_view offered,
    http_accept_token_match_mode mode) noexcept {
    detail::http_accumulate_token_acceptance(
        field, offered, mode == http_accept_token_match_mode::language_prefix, specificity_, quality_);
}

}  // namespace ruvia

namespace ruvia {
namespace {
using item_type = detail::http_structured_item;

template <typename input_type>
std::variant<std::monostate, http_priority_error> parse_priority_members(detail::http_structured_parser<input_type> parser, http_priority_fields& fields_value) noexcept {
    parser.spaces();
    while (!parser.empty()) {
        const auto key = parser.key();
        if (key.empty()) {
            return http_priority_error::invalid_syntax;
        }
        item_type item{.kind_ = item_type::kind_type::boolean, .boolean_ = true};
        if (parser.take('=')) {
            item = {};
            if (!parser.member(item)) {
                return http_priority_error::invalid_syntax;
            }
        } else if (!parser.parameters()) {
            return http_priority_error::invalid_syntax;
        }
        if (key == "u") {
            fields_value.urgency_ = item.kind_ == item_type::kind_type::integer
                                        ? http_priority_urgency::try_from_value(item.integer_)
                                        : std::nullopt;
        }
        if (key == "i") {
            fields_value.incremental_ = item.kind_ == item_type::kind_type::boolean ? std::optional(item.boolean_) : std::nullopt;
        }
        parser.ows();
        if (parser.empty()) {
            break;
        }
        if (!parser.take(',')) {
            return http_priority_error::invalid_syntax;
        }
        parser.ows();
        if (parser.empty()) {
            return http_priority_error::invalid_syntax;
        }
    }
    return {};
}
}  // namespace

std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::string_view value) noexcept {
    http_priority_fields fields;
    if (auto parsed_value = parse_priority_members(detail::http_structured_parser{detail::http_structured_text_input{value}}, fields); parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    return fields;
}
namespace detail {
std::variant<http_priority_fields, http_priority_error> parse_http_priority_headers(
    http_header_range_view headers) noexcept {
    http_priority_fields fields;
    if (auto parsed_value = parse_priority_members(
            http_structured_parser{
                http_structured_header_input{headers.begin(), headers.end(), "priority"}},
            fields);
        parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    return fields;
}
}  // namespace detail
std::variant<std::size_t, http_priority_error> encode_http_priority(std::span<char> output, http_priority_fields fields_value) noexcept {
    std::array<char, 12> bytes_value{};
    std::size_t count = 0;
    if (fields_value.urgency_) {
        bytes_value[count++] = 'u';
        bytes_value[count++] = '=';
        bytes_value[count++] = static_cast<char>('0' + fields_value.urgency_->value());
    }
    if (fields_value.incremental_) {
        if (*fields_value.incremental_) {
            if (count) {
                bytes_value[count++] = ',';
                bytes_value[count++] = ' ';
            }
            bytes_value[count++] = 'i';
        } else {
            if (count) {
                bytes_value[count++] = ',';
            }
            bytes_value[count++] = 'i';
            bytes_value[count++] = '=';
            bytes_value[count++] = '?';
            bytes_value[count++] = '0';
        }
    }
    if (output.size() < count) {
        return http_priority_error::output_too_small;
    }
    std::copy_n(bytes_value.begin(), count, output.begin());
    return count;
}
std::variant<http_priority_update, http_priority_error> decode_http2_priority_update(std::span<const char> payload_value) noexcept {
    if (payload_value.size() < 4) {
        return http_priority_error::invalid_frame;
    }
    const auto* id_bytes = reinterpret_cast<const unsigned char*>(payload_value.data());
    const auto id = ((static_cast<std::uint32_t>(id_bytes[0]) & 0x7fU) << 24) |
                    (static_cast<std::uint32_t>(id_bytes[1]) << 16) |
                    (static_cast<std::uint32_t>(id_bytes[2]) << 8) | static_cast<std::uint32_t>(id_bytes[3]);
    if (!id) {
        return http_priority_error::invalid_value;
    }
    auto fields_value = parse_http_priority({payload_value.data() + 4, payload_value.size() - 4});
    if ((fields_value.index() != 0)) {
        return std::get<1>(fields_value);
    }
    return http_priority_update{id, (id & 1U) == 0, std::get<0>(fields_value)};
}
std::variant<std::size_t, http_priority_error> encode_http2_priority_update(std::span<char> output, std::uint32_t id, http_priority_fields fields_value) noexcept {
    if (!id || id > 0x7fffffffU) {
        return http_priority_error::invalid_value;
    }
    std::array<char, 12> value{};
    const auto length = encode_http_priority(value, fields_value);
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    if (output.size() < 13 + std::get<0>(length)) {
        return http_priority_error::output_too_small;
    }
    (void)encode_http2_frame_header(output.first(http2_frame_header_bytes),
        static_cast<std::uint32_t>(4 + std::get<0>(length)), http2_frame_type::priority_update, 0, 0);
    for (std::size_t index = 0; index < 4; ++index) {
        output[http2_frame_header_bytes + index] = static_cast<char>((id >> (24 - 8 * index)) & 0xffU);
    }
    std::copy_n(value.begin(), std::get<0>(length), output.begin() + 13);
    return 13 + std::get<0>(length);
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_valid_http_cors_request_header_names(std::string_view value) noexcept {
    http_field_name_list names(value);
    bool saw_name = false;
    while (names.next()) {
        saw_name = true;
    }
    // Access-Control-Request-Headers requires at least one field name, unlike
    // the general #field-name grammar.
    return saw_name && names.valid();
}

// Host is a once-only field (RFC 9112 section 3.2) without a cached singleton
// bit; its kind index is below 32 like every singleton kind.
[[nodiscard]] constexpr std::uint32_t once_only_bit(http_field_kind kind) noexcept {
    return kind == http_field_kind::host ? 1U << static_cast<std::uint32_t>(kind)
                                         : singleton_request_header_bit(kind);
}

// RFC 9110 section 5.6.6 parameter: no whitespace around '='.
[[nodiscard]] bool is_exact_http_parameter(std::string_view part) noexcept {
    const auto equals = part.find('=');
    return equals != std::string_view::npos && is_valid_http_header_name(part.substr(0, equals)) &&
           is_http_token_or_quoted_string(part.substr(equals + 1));
}

[[nodiscard]] bool http_expectation_item(std::string_view item) noexcept {
    item = http_trim_ows(item);
    if (item.empty()) {
        return false;
    }

    const auto parameters_start = http_find_unquoted_delimiter(item, 0, ';');
    const auto leading = http_trim_ows(item.substr(0, parameters_start));
    if (leading.find('=') == std::string_view::npos) {
        return parameters_start == item.size() && is_valid_http_header_name(leading);
    }
    // RFC 9110 section 5.6.6: parameters = *( OWS ";" OWS [ parameter ] ).
    return is_exact_http_parameter(leading) && http_all_parameters(item, [](std::string_view parameter) noexcept {
        return parameter.empty() || is_exact_http_parameter(parameter);
    });
}

// Recipient-side Expect grammar. Empty list members are ignored, but every
// non-empty member still has to be a syntactically valid expectation before
// product policy decides whether unsupported extensions are rejected with 417
// or ignored.
[[nodiscard]] bool is_valid_received_http_expect_field_value(std::string_view value) noexcept {
    return http_visit_list_members(value, http_field_list_role::recipient, [](std::string_view item) noexcept {
        return http_expectation_item(item) ? http_field_list_parse_status::ok : http_field_list_parse_status::malformed;
    }) == http_field_list_parse_status::ok;
}

[[nodiscard]] bool is_valid_te(
    std::string_view value, http_field_list_role role, request_field_framing framing) noexcept {
    if (framing == request_field_framing::binary) {
        return is_valid_binary_http_te_field_value(value, role);
    }
    return is_valid_http_te_field_value(value, role);
}

[[nodiscard]] bool is_valid_value(http_field_kind kind, std::string_view value,
    http_field_list_role role, request_field_framing framing, http_request_field_source source) noexcept {
    // RFC 9110 sections 2.2 and 5.6.1: forwarding is not generation. Reuse the
    // recipient value rules only for end-to-end application metadata. Ownership
    // and singleton checks below still use the actual sender role.
    const auto application_role = source == http_request_field_source::forwarded
                                      ? http_field_list_role::recipient
                                      : role;
    switch (kind) {
        case http_field_kind::origin:
            return application_role == http_field_list_role::recipient || is_valid_http_origin_field_value(value);
        case http_field_kind::access_control_request_method:
            return application_role == http_field_list_role::recipient || is_valid_http_method_token(value);
        case http_field_kind::access_control_request_headers:
            return application_role == http_field_list_role::recipient || is_valid_http_cors_request_header_names(value);
        case http_field_kind::content_type:
            // Recipients interpret media types at the application boundary.
            return application_role == http_field_list_role::recipient ||
                   is_valid_http_content_type_field_value(value);
        case http_field_kind::content_encoding:
            // Admission leaves received metadata to the decoder. Forwarding
            // validates the recipient list grammar, not arbitrary nonempty text.
            return role == http_field_list_role::recipient ||
                   is_valid_http_content_encoding_field_value(value, application_role);
        case http_field_kind::trailer:
            // The declaration neither frames content nor authorizes actual trailers.
            return role == http_field_list_role::recipient ||
                   is_valid_http_request_trailer_field_value(value, role);
        case http_field_kind::te:
            return is_valid_te(value, role, framing);
        case http_field_kind::expect:
            // Binary field semantics feed the existing 417/ignore policy, not malformed-message rejection.
            return framing == request_field_framing::binary ||
                   is_valid_received_http_expect_field_value(value);
        case http_field_kind::host:
            return is_valid_host_header(value);
        default:
            return true;
    }
}

}  // namespace

bool request_field_state::seen(http_field_kind kind) const noexcept {
    return (seen_ & once_only_bit(kind)) != 0;
}

request_field_status validate_request_field(http_field_kind kind, std::string_view value,
    http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept {
    if (!is_valid_http_request_field_source(source)) {
        return request_field_status::invalid;
    }
    if (role == http_field_list_role::sender &&
        (kind == http_field_kind::expect || kind == http_field_kind::content_length)) {
        return request_field_status::managed_by_sender;
    }
    if (!is_valid_value(kind, value, role, state.framing_, source)) {
        return request_field_status::invalid;
    }
    // Invalid date conditions, including repeated field lines, are ignored
    // rather than rejecting the request (RFC 9110 sections 13.1.3 and 13.1.4).
    // A repeated User-Agent violates only the sender rule of RFC 9110 section
    // 5.3 and controls no framing, routing or access, so recipients keep its
    // lines. Senders still cannot generate these singleton fields more than once.
    if (role == http_field_list_role::recipient &&
        (kind == http_field_kind::if_modified_since ||
            kind == http_field_kind::if_unmodified_since || kind == http_field_kind::user_agent)) {
        return request_field_status::ok;
    }
    if (const auto bit = once_only_bit(kind); bit != 0) {
        if ((state.seen_ & bit) != 0) {
            return request_field_status::invalid;
        }
        state.seen_ |= bit;
    }
    return request_field_status::ok;
}

bool is_protocol_managed_request_field(http_field_kind kind, std::string_view name) noexcept {
    switch (kind) {
        case http_field_kind::host:
        case http_field_kind::content_length:
        case http_field_kind::transfer_encoding:
        case http_field_kind::trailer:
        case http_field_kind::expect:
        case http_field_kind::connection:
        case http_field_kind::te:
        case http_field_kind::upgrade:
            return true;
        case http_field_kind::other:
            // Keep-Alive and Proxy-Connection have no request identity.
            return is_forbidden_http_binary_connection_field(name);
        default:
            return false;
    }
}

}  // namespace ruvia::detail

namespace ruvia {

bool is_http_protocol_managed_request_field(std::string_view name) noexcept {
    return detail::is_protocol_managed_request_field(detail::classify_http_field(name), name);
}

bool is_valid_http_request_application_field(std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    const auto kind = detail::classify_http_field(name);
    if (detail::is_protocol_managed_request_field(kind, name)) {
        return false;
    }
    // TE 是唯一按 HTTP 版本区分的请求字段语义，已由托管字段边界排除。
    // 每次只检查一个字段；重复检测由调用方持有的字段集合负责。
    detail::request_field_state state(detail::request_field_framing::http1);
    return detail::validate_request_field(kind, value, detail::http_field_list_role::sender, state) ==
           detail::request_field_status::ok;
}

std::optional<std::string_view> http_field_name_list::next() noexcept {
    while (!finished_) {
        const auto separator = remaining_.find(',');
        const auto member = remaining_.substr(0, separator);
        if (separator == std::string_view::npos) {
            finished_ = true;
        } else {
            remaining_.remove_prefix(separator + 1);
        }
        const auto name = http_trim_ows(member);
        if (name.empty()) {
            if (finished_) {
                valid_ = true;
            }
            continue;
        }
        if (!is_valid_http_header_name(name)) {
            finished_ = true;
            valid_ = false;
            return std::nullopt;
        }
        if (finished_) {
            valid_ = true;
        }
        return name;
    }
    return std::nullopt;
}

void http_request_expectations::parse_field(std::string_view value) noexcept {
    detail::http_visit_comma_separated_quoted(value, [this](std::string_view member) noexcept {
        if (http_ascii_equals_ignore_case(member, detail::http_continue_expectation_token)) {
            flags_ |= continue_value;
        } else {
            flags_ |= unsupported;
        }
        return true;
    });
}
}  // namespace ruvia

namespace ruvia::detail {

bool is_valid_long_http_field_value_bytes(const char* data, std::size_t size) noexcept {
    return http_field_value_prefix_size({data, size}) == size;
}

bool is_forbidden_common_trailer_name(
    std::string_view name, http_field_kind kind) noexcept {
    switch (kind) {
        case http_field_kind::host:
        case http_field_kind::content_length:
        case http_field_kind::transfer_encoding:
        case http_field_kind::connection:
        case http_field_kind::content_encoding:
        case http_field_kind::content_type:
        case http_field_kind::cookie:
        case http_field_kind::expect:
        case http_field_kind::if_match:
        case http_field_kind::if_modified_since:
        case http_field_kind::if_none_match:
        case http_field_kind::if_range:
        case http_field_kind::if_unmodified_since:
        case http_field_kind::range:
        case http_field_kind::upgrade:
        case http_field_kind::authorization:
        case http_field_kind::te:
        case http_field_kind::trailer:
        case http_field_kind::proxy_authorization:
            return true;
        case http_field_kind::other:
        case http_field_kind::accept:
        case http_field_kind::accept_encoding:
        case http_field_kind::access_control_request_headers:
        case http_field_kind::access_control_request_method:
        case http_field_kind::origin:
        case http_field_kind::user_agent:
        case http_field_kind::sec_websocket_key:
        case http_field_kind::sec_websocket_protocol:
        case http_field_kind::sec_websocket_version:
        case http_field_kind::forwarded:
        case http_field_kind::x_forwarded_for:
        case http_field_kind::x_forwarded_proto:
        case http_field_kind::sec_websocket_extensions:
            break;
    }

    switch (name.size()) {
        case 10:
            return http_ascii_equals_ignore_case(name, "Keep-Alive") ||
                   http_ascii_equals_ignore_case(name, "Set-Cookie");
        case 12:
            return http_ascii_equals_ignore_case(name, "Max-Forwards");
        case 13:
            return http_ascii_equals_ignore_case(name, "Cache-Control") ||
                   http_ascii_equals_ignore_case(name, "Content-Range");
        case 16:
            return http_ascii_equals_ignore_case(name, "Proxy-Connection");
        case 18:
            return http_ascii_equals_ignore_case(name, "Proxy-Authenticate");
        default:
            return false;
    }
}

bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept {
    const auto kind = classify_http_field(name);
    if (is_forbidden_common_trailer_name(name, kind)) {
        return true;
    }
    return kind == http_field_kind::access_control_request_headers ||
           kind == http_field_kind::access_control_request_method ||
           kind == http_field_kind::origin ||
           http_ascii_equals_ignore_case(name, "Accept-Ranges");
}

bool is_forbidden_response_trailer_name(std::string_view name) noexcept {
    // The response header classifier is the authoritative set of standardized
    // fields Ruvia manages. RFC 9110 explicitly permits only ETag (section
    // 8.8.3) and Accept-Ranges (section 14.3) from that set in trailers; every
    // other known field lacks trailer permission or controls framing,
    // representation handling, caching, routing, cookies, methods, or CORS.
    if (const auto known = classify_response_header_name(name);
        known != 0 && known != response_header_etag && known != response_header_accept_ranges) {
        return true;
    }

    if (is_forbidden_common_trailer_name(name, classify_http_field(name))) {
        return true;
    }

    switch (name.size()) {
        case 3:
            // Response control data (RFC 9110 §7.4).
            return http_ascii_equals_ignore_case(name, "Age");
        case 6:
            return http_ascii_equals_ignore_case(name, "Pragma");
        case 7:
            return http_ascii_equals_ignore_case(name, "Expires") ||
                   http_ascii_equals_ignore_case(name, "Warning");
        case 11:
            return http_ascii_equals_ignore_case(name, "Retry-After");
        case 15:
            return http_ascii_equals_ignore_case(name, "X-Frame-Options") ||
                   http_ascii_equals_ignore_case(name, "Referrer-Policy") ||
                   http_ascii_equals_ignore_case(name, "Clear-Site-Data");
        case 16:
            return http_ascii_equals_ignore_case(name, "X-XSS-Protection") ||
                   http_ascii_equals_ignore_case(name, "WWW-Authenticate");
        case 18:
            return http_ascii_equals_ignore_case(name, "Permissions-Policy");
        case 19:
            return http_ascii_equals_ignore_case(name, "Content-Disposition");
        case 22:
            return http_ascii_equals_ignore_case(name, "X-Content-Type-Options");
        case 23:
            return http_ascii_equals_ignore_case(name, "Content-Security-Policy");
        case 25:
            return http_ascii_equals_ignore_case(name, "Strict-Transport-Security");
        case 35:
            return http_ascii_equals_ignore_case(name, "Content-Security-Policy-Report-Only");
        default:
            return false;
    }
}

bool is_forbidden_http_binary_response_field(std::string_view name) noexcept {
    return is_forbidden_http_binary_connection_field(name) || http_ascii_equals_ignore_case(name, "te");
}

[[nodiscard]] bool is_valid_http_response_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_response_trailer_name(name); });
}

[[nodiscard]] bool response_trailer_field_valid(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && !is_forbidden_response_trailer_name(name) &&
           is_valid_http_header_value(value);
}

response_field_status validate_response_field(http_field_kind kind, std::string_view value,
    response_field_state& state) noexcept {
    switch (kind) {
        case http_field_kind::content_type:
            if (!is_valid_http_content_type_field_value(value)) {
                return response_field_status::invalid;
            }
            if (state.content_type_seen_) {
                return response_field_status::repeated_singleton;
            }
            state.content_type_seen_ = true;
            break;
        case http_field_kind::content_encoding:
            if (!is_valid_http_content_encoding_field_value(value, http_field_list_role::sender)) {
                return response_field_status::invalid;
            }
            break;
        case http_field_kind::trailer:
            if (!is_valid_http_response_trailer_field_value(value, http_field_list_role::sender)) {
                return response_field_status::invalid;
            }
            break;
        default:
            break;
    }
    return response_field_status::ok;
}

}  // namespace ruvia::detail

namespace ruvia {

bool is_valid_http_response_field(std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    detail::response_field_state state;
    return detail::validate_response_field(detail::classify_http_field(name), value, state) ==
           detail::response_field_status::ok;
}

http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers) {
    auto section_size = http_trailer_section_budget();
    for (const auto& trailer : trailers) {
        if (!detail::response_trailer_field_valid(trailer.name(), trailer.value()) ||
            !section_size.add_field(trailer.name(), trailer.value())) {
            throw std::invalid_argument("invalid HTTP response trailer section");
        }
    }
    return http_response_trailer_section(trailers);
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Parse a delta-seconds value (RFC 9111 section 1.2.2): an optionally DQUOTE-wrapped non-negative
// integer. Quoted-pairs have their RFC 9110 section 5.6.4 semantic value, so
// `"6\0"` is equivalent to `60`. Syntactically valid overflow saturates to the
// greatest convenient representation; non-digit / empty input remains invalid.
[[nodiscard]] std::optional<std::uint64_t> parse_delta_seconds(std::string_view value) noexcept {
    std::uint64_t result_value = 0;
    bool any_digit = false;
    const auto accumulate = [&result_value, &any_digit](unsigned char ch) noexcept {
        if (ch < '0' || ch > '9') {
            return false;
        }
        constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        result_value = result_value > (maximum - digit) / 10 ? maximum : result_value * 10 + digit;
        any_digit = true;
        return true;
    };
    if (!value.empty() && value.front() == '"') {
        http_quoted_string_cursor cursor(value);
        unsigned char ch = 0;
        while (cursor.next(ch)) {
            if (!accumulate(ch)) {
                return std::nullopt;
            }
        }
        if (!cursor.closed() || cursor.consumed() != value.size()) {
            return std::nullopt;
        }
    } else {
        for (const auto ch : value) {
            if (!accumulate(static_cast<unsigned char>(ch))) {
                return std::nullopt;
            }
        }
    }
    if (!any_digit) {
        return std::nullopt;
    }
    return result_value;
}

[[nodiscard]] bool parse_nominated_field_list_arg(
    std::string_view arg, auto&& add_field) noexcept {
    if (arg.empty()) {
        return false;
    }
    std::array<char, 256> unescaped_buf{};
    std::string_view list_text = arg;
    if (arg.front() == '"') {
        http_quoted_string_cursor cursor(arg);
        std::size_t len = 0;
        unsigned char ch = 0;
        while (cursor.next(ch)) {
            if (len >= unescaped_buf.size()) {
                return false;
            }
            unescaped_buf[len++] = static_cast<char>(ch);
        }
        if (!cursor.closed() || cursor.consumed() != arg.size()) {
            return false;
        }
        list_text = std::string_view(unescaped_buf.data(), len);
    } else if (!is_valid_http_header_name(arg)) {
        return false;
    }

    http_field_name_list names(list_text);
    bool saw_name = false;
    while (const auto name = names.next()) {
        saw_name = true;
        if (!add_field(*name)) {
            return false;
        }
    }
    return saw_name && names.valid();
}
}  // namespace

void cache_control_field_parser::update(std::string_view value) noexcept {
    detail::http_visit_comma_separated_quoted(value, [this](std::string_view token) noexcept {
        const auto eq = token.find('=');
        const bool has_argument = eq != std::string_view::npos;
        const auto name = eq == std::string_view::npos ? token : token.substr(0, eq);
        const auto arg = eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);
        switch (name.size()) {
            case 6U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "public")) {
                    value_.set(cache_control_directive::public_value);
                }
                break;
            case 7U:
                if (http_ascii_equals_ignore_case(name, "max-age")) {
                    if (!max_age_seen_) {
                        max_age_seen_ = true;
                        value_.max_age_ = parse_delta_seconds(arg);
                    }
                } else if (http_ascii_equals_ignore_case(name, "private")) {
                    if (!has_argument) {
                        value_.set(cache_control_directive::private_value);
                        value_.set(cache_control_directive::private_unqualified);
                        value_.private_fields_.clear();
                    } else if (detail::is_http_token_or_quoted_string(arg)) {
                        value_.set(cache_control_directive::private_value);
                        if (!value_.is_unqualified_private()) {
                            const bool added = parse_nominated_field_list_arg(
                                arg, [this](std::string_view field_name) noexcept {
                                    return value_.private_fields_.add_lowercase(field_name);
                                });
                            if (!added) {
                                value_.set(cache_control_directive::private_unqualified);
                                value_.private_fields_.clear();
                            }
                        }
                    }
                }
                break;
            case 8U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "no-store")) {
                    value_.set(cache_control_directive::no_store);
                } else if (http_ascii_equals_ignore_case(name, "no-cache")) {
                    if (!has_argument) {
                        value_.set(cache_control_directive::no_cache);
                        value_.set(cache_control_directive::no_cache_unqualified);
                        value_.no_cache_fields_.clear();
                    } else if (detail::is_http_token_or_quoted_string(arg)) {
                        value_.set(cache_control_directive::no_cache);
                        if (!value_.is_unqualified_no_cache()) {
                            const bool added = parse_nominated_field_list_arg(
                                arg, [this](std::string_view field_name) noexcept {
                                    return value_.no_cache_fields_.add_lowercase(field_name);
                                });
                            if (!added) {
                                value_.set(cache_control_directive::no_cache_unqualified);
                                value_.no_cache_fields_.clear();
                            }
                        }
                    }
                } else if (http_ascii_equals_ignore_case(name, "s-maxage")) {
                    if (!s_max_age_seen_) {
                        s_max_age_seen_ = true;
                        value_.s_max_age_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 9U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "immutable")) {
                    value_.set(cache_control_directive::immutable);
                } else if (http_ascii_equals_ignore_case(name, "max-stale")) {
                    if (!max_stale_seen_) {
                        max_stale_seen_ = true;
                        if (!has_argument) {
                            value_.set(cache_control_directive::max_stale_any);
                        } else {
                            value_.max_stale_ = parse_delta_seconds(arg);
                        }
                    }
                } else if (http_ascii_equals_ignore_case(name, "min-fresh")) {
                    if (!min_fresh_seen_) {
                        min_fresh_seen_ = true;
                        value_.min_fresh_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 12U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "no-transform")) {
                    value_.set(cache_control_directive::no_transform);
                }
                break;
            case 14U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "only-if-cached")) {
                    value_.set(cache_control_directive::only_if_cached);
                } else if (http_ascii_equals_ignore_case(name, "stale-if-error")) {
                    if (!stale_if_error_seen_) {
                        stale_if_error_seen_ = true;
                        value_.stale_if_error_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 15U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "must-revalidate")) {
                    value_.set(cache_control_directive::must_revalidate);
                }
                break;
            case 16U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "proxy-revalidate")) {
                    value_.set(cache_control_directive::proxy_revalidate);
                }
                break;
            case 22U:
                if (http_ascii_equals_ignore_case(name, "stale-while-revalidate")) {
                    if (!stale_while_revalidate_seen_) {
                        stale_while_revalidate_seen_ = true;
                        value_.stale_while_revalidate_ = parse_delta_seconds(arg);
                    }
                }
                break;
            default:
                break;
        }
        return true;
    });
}

cache_control parse_cache_control(std::string_view value) noexcept {
    cache_control_field_parser parser;
    parser.update(value);
    return parser.finish();
}

cache_control detail::parse_cache_control_headers(detail::http_header_range_view headers) noexcept {
    cache_control_field_parser parser;
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            parser.update(field.value());
        }
    }
    return parser.finish();
}

bool http_cache_control_has_no_transform(std::string_view field_value) noexcept {
    return parse_cache_control(field_value).no_transform();
}

bool cache_control::nominated_field_set::contains(std::string_view field_name) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (http_ascii_equals_ignore_case(at(i), field_name)) {
            return true;
        }
    }
    return false;
}

bool cache_control::nominated_field_set::add_lowercase(std::string_view field_name) noexcept {
    if (field_name.empty() || field_name.size() > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    if (contains(field_name)) {
        return true;
    }
    if (count_ >= max_nominated_fields ||
        field_name.size() > max_nominated_field_bytes - storage_used_) {
        return false;
    }
    offsets_[count_] = storage_used_;
    lengths_[count_] = static_cast<std::uint8_t>(field_name.size());
    for (std::size_t i = 0; i < field_name.size(); ++i) {
        storage_[storage_used_ + i] =
            static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(field_name[i])));
    }
    storage_used_ = static_cast<std::uint16_t>(storage_used_ + field_name.size());
    ++count_;
    return true;
}

bool cache_control::has_private_field(std::string_view field_name) const noexcept {
    return !is_unqualified_private() && private_fields_.contains(field_name);
}

bool cache_control::has_no_cache_field(std::string_view field_name) const noexcept {
    return !is_unqualified_no_cache() && no_cache_fields_.contains(field_name);
}

template <typename input_type>
std::optional<cache_control> parse_targeted_cache_control_from_input(input_type input) noexcept {
    detail::http_structured_parser<input_type> parser{std::move(input)};
    parser.ows();
    if (parser.empty()) {
        return std::nullopt;
    }
    cache_control result;
    bool saw_member = false;
    while (!parser.empty()) {
        const auto key = parser.key();
        if (key.empty()) {
            return std::nullopt;
        }
        saw_member = true;
        detail::http_structured_item item{
            .kind_ = detail::http_structured_item::kind_type::boolean,
            .boolean_ = true,
        };
        if (parser.take('=')) {
            item = {};
            if (!parser.member(item)) {
                return std::nullopt;
            }
        } else if (!parser.parameters()) {
            return std::nullopt;
        }

        const auto apply_bool_directive = [&](cache_control_directive dir) noexcept {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean) {
                if (item.boolean_) {
                    result.set(dir);
                } else {
                    result.clear_directive(dir);
                }
            } else {
                result.clear_directive(dir);
            }
        };
        const auto apply_delta_directive =
            [&](std::optional<std::uint64_t>& target) noexcept {
                if (item.kind_ == detail::http_structured_item::kind_type::integer &&
                    item.integer_ >= 0) {
                    target = static_cast<std::uint64_t>(item.integer_);
                } else {
                    target.reset();
                }
            };
        const auto apply_qualified_directive = [&](cache_control_directive flag_dir,
                                                   cache_control_directive unqual_dir,
                                                   cache_control::nominated_field_set& fields) noexcept {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean) {
                fields.clear();
                if (item.boolean_) {
                    result.set(flag_dir);
                    result.set(unqual_dir);
                } else {
                    result.clear_directive(flag_dir);
                    result.clear_directive(unqual_dir);
                }
            } else if (item.kind_ == detail::http_structured_item::kind_type::string ||
                       item.kind_ == detail::http_structured_item::kind_type::token) {
                fields.clear();
                result.clear_directive(unqual_dir);
                result.set(flag_dir);
                std::array<char, 256> unescaped{};
                std::string_view list_text = item.text_;
                if (item.kind_ == detail::http_structured_item::kind_type::string &&
                    item.has_escapes_) {
                    std::size_t out_len = 0;
                    for (std::size_t i = 0; i < item.text_.size(); ++i) {
                        if (item.text_[i] == '\\' && i + 1U < item.text_.size()) {
                            ++i;
                        }
                        if (out_len >= unescaped.size()) {
                            result.set(unqual_dir);
                            fields.clear();
                            return;
                        }
                        unescaped[out_len++] = item.text_[i];
                    }
                    list_text = std::string_view(unescaped.data(), out_len);
                }
                http_field_name_list names(list_text);
                bool saw_name = false;
                bool ok = true;
                while (const auto field_name = names.next()) {
                    saw_name = true;
                    if (!fields.add_lowercase(*field_name)) {
                        ok = false;
                        break;
                    }
                }
                if (!saw_name || !names.valid() || !ok) {
                    result.set(unqual_dir);
                    fields.clear();
                }
            } else {
                fields.clear();
                result.clear_directive(flag_dir);
                result.clear_directive(unqual_dir);
            }
        };

        if (key == "no-store") {
            apply_bool_directive(cache_control_directive::no_store);
        } else if (key == "no-transform") {
            apply_bool_directive(cache_control_directive::no_transform);
        } else if (key == "must-revalidate") {
            apply_bool_directive(cache_control_directive::must_revalidate);
        } else if (key == "proxy-revalidate") {
            apply_bool_directive(cache_control_directive::proxy_revalidate);
        } else if (key == "public") {
            apply_bool_directive(cache_control_directive::public_value);
        } else if (key == "immutable") {
            apply_bool_directive(cache_control_directive::immutable);
        } else if (key == "only-if-cached") {
            apply_bool_directive(cache_control_directive::only_if_cached);
        } else if (key == "private") {
            apply_qualified_directive(cache_control_directive::private_value,
                cache_control_directive::private_unqualified, result.private_fields_);
        } else if (key == "no-cache") {
            apply_qualified_directive(cache_control_directive::no_cache,
                cache_control_directive::no_cache_unqualified, result.no_cache_fields_);
        } else if (key == "max-age") {
            apply_delta_directive(result.max_age_);
        } else if (key == "s-maxage") {
            apply_delta_directive(result.s_max_age_);
        } else if (key == "stale-while-revalidate") {
            apply_delta_directive(result.stale_while_revalidate_);
        } else if (key == "stale-if-error") {
            apply_delta_directive(result.stale_if_error_);
        } else if (key == "min-fresh") {
            apply_delta_directive(result.min_fresh_);
        } else if (key == "max-stale") {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean && item.boolean_) {
                result.set(cache_control_directive::max_stale_any);
                result.max_stale_.reset();
            } else if (item.kind_ == detail::http_structured_item::kind_type::integer &&
                       item.integer_ >= 0) {
                result.clear_directive(cache_control_directive::max_stale_any);
                result.max_stale_ = static_cast<std::uint64_t>(item.integer_);
            } else {
                result.clear_directive(cache_control_directive::max_stale_any);
                result.max_stale_.reset();
            }
        }

        parser.ows();
        if (parser.empty()) {
            break;
        }
        if (!parser.take(',')) {
            return std::nullopt;
        }
        parser.ows();
        if (parser.empty()) {
            return std::nullopt;
        }
    }
    if (!saw_member) {
        return std::nullopt;
    }
    return result;
}

std::optional<cache_control> parse_targeted_cache_control(std::string_view value) noexcept {
    return parse_targeted_cache_control_from_input(detail::http_structured_text_input{value});
}

std::optional<cache_control> detail::parse_targeted_cache_control_headers(
    detail::http_header_range_view headers, std::string_view header_name) noexcept {
    if (header_name.empty()) {
        return std::nullopt;
    }
    return parse_targeted_cache_control_from_input(
        detail::http_structured_header_input{headers.begin(), headers.end(), header_name});
}

targeted_cache_control_selection detail::resolve_targeted_cache_control_headers(
    detail::http_header_range_view headers,
    std::span<const std::string_view> target_headers) noexcept {
    const auto has_cache_control = [&headers]() noexcept {
        for (const auto field : headers) {
            if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
                return true;
            }
        }
        return false;
    };
    for (const auto target_name : target_headers) {
        if (target_name.empty()) {
            continue;
        }
        if (http_ascii_equals_ignore_case(target_name, "Cache-Control")) {
            if (has_cache_control()) {
                return {
                    .control_ = parse_cache_control_headers(headers),
                    .selected_header_ = target_name,
                    .targeted_ = false,
                };
            }
            continue;
        }
        if (const auto targeted = parse_targeted_cache_control_headers(headers, target_name)) {
            return {
                .control_ = *targeted,
                .selected_header_ = target_name,
                .targeted_ = true,
            };
        }
    }
    return {
        .control_ = parse_cache_control_headers(headers),
        .selected_header_ = has_cache_control() ? std::string_view("Cache-Control")
                                                : std::string_view{},
        .targeted_ = false,
    };
}

bool strip_http_cache_private_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    if (control.is_field_qualified_private()) {
        std::erase_if(headers, [&control](const http_header& field) noexcept {
            return control.has_private_field(field.name());
        });
    }
    return true;
}

bool strip_http_cache_private_headers(
    http_response_headers& headers, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    const auto count = control.private_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove_header(control.private_field(i));
    }
    return true;
}

bool strip_http_cache_private_headers(
    http_response& response, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    if (!control.is_field_qualified_private()) {
        return true;
    }
    auto staged = response.headers().clone();
    (void)strip_http_cache_private_headers(staged, control);
    response.commit_headers(std::move(staged));
    return true;
}

bool strip_http_cache_private_headers(std::vector<http_header>& headers) noexcept {
    return strip_http_cache_private_headers(headers, parse_cache_control(headers));
}

bool strip_http_cache_private_headers(http_response_headers& headers) {
    return strip_http_cache_private_headers(headers, parse_cache_control(headers));
}

bool strip_http_cache_private_headers(http_response& response) {
    return strip_http_cache_private_headers(response, parse_cache_control(response));
}

bool strip_http_cache_no_cache_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    if (control.is_field_qualified_no_cache()) {
        std::erase_if(headers, [&control](const http_header& field) noexcept {
            return control.has_no_cache_field(field.name());
        });
    }
    return true;
}

bool strip_http_cache_no_cache_headers(
    http_response_headers& headers, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    const auto count = control.no_cache_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove_header(control.no_cache_field(i));
    }
    return true;
}

bool strip_http_cache_no_cache_headers(
    http_response& response, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    if (!control.is_field_qualified_no_cache()) {
        return true;
    }
    auto staged = response.headers().clone();
    (void)strip_http_cache_no_cache_headers(staged, control);
    response.commit_headers(std::move(staged));
    return true;
}

namespace {

void append_preserved_cache_control_directives(
    std::string_view field_value, std::string& output) {
    detail::http_visit_comma_separated_quoted(field_value, [&](std::string_view token) {
        const auto eq = token.find('=');
        const auto name =
            http_trim_ows(eq == std::string_view::npos ? token : token.substr(0, eq));
        if (name.empty() || http_ascii_equals_ignore_case(name, "max-age") ||
            http_ascii_equals_ignore_case(name, "s-maxage")) {
            return true;
        }
        if (!output.empty()) {
            output.append(", ");
        }
        output.append(token);
        return true;
    });
}

void finish_rewritten_cache_control_max_age(
    std::string& output, std::uint64_t client_max_age) {
    if (!output.empty()) {
        output.append(", ");
    }
    output.append("max-age=");
    http_append_unsigned(output, client_max_age);
}

[[nodiscard]] std::string normalize_vary_field_value(std::string_view raw) {
    const auto trimmed = http_trim_ows(raw);
    std::string normalized;
    normalized.reserve(trimmed.size());
    bool in_ows = false;
    for (const char ch : trimmed) {
        if (ch == ' ' || ch == '\t') {
            if (!in_ows) {
                normalized.push_back(' ');
                in_ows = true;
            }
        } else {
            normalized.push_back(ch);
            in_ows = false;
        }
    }
    return normalized;
}

template <typename target_headers_type>
void merge_304_headers_impl(
    target_headers_type& cached_headers, detail::http_header_range_view not_modified_headers) {
    const auto nominated = parse_http_connection_nominated_fields(not_modified_headers);
    std::vector<std::string_view> updated_names;
    for (const auto field : not_modified_headers) {
        const auto name = field.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(field.value()) ||
            is_http_hop_by_hop_or_nominated_field(name, nominated) ||
            is_http_304_forbidden_header(name)) {
            continue;
        }
        const bool already_recorded =
            std::ranges::any_of(updated_names, [name](std::string_view existing) noexcept {
                return http_ascii_equals_ignore_case(existing, name);
            });
        if (!already_recorded) {
            updated_names.push_back(name);
        }
    }
    if (updated_names.empty()) {
        return;
    }
    if constexpr (std::same_as<target_headers_type, http_response_headers>) {
        for (const auto name : updated_names) {
            cached_headers.remove_header(name);
        }
    } else {
        std::erase_if(cached_headers, [&](const http_header& existing) noexcept {
            return std::ranges::any_of(updated_names, [&](std::string_view updated) noexcept {
                return http_ascii_equals_ignore_case(existing.name(), updated);
            });
        });
    }
    for (const auto field : not_modified_headers) {
        const auto name = field.name();
        const bool should_copy =
            std::ranges::any_of(updated_names, [name](std::string_view updated) noexcept {
                return http_ascii_equals_ignore_case(name, updated);
            });
        if (!should_copy) {
            continue;
        }
        if constexpr (std::same_as<target_headers_type, http_response_headers>) {
            if (cached_headers.header(name).has_value() &&
                !http_ascii_equals_ignore_case(name, "Date") &&
                !http_ascii_equals_ignore_case(name, "ETag") &&
                !http_ascii_equals_ignore_case(name, "Expires") &&
                !http_ascii_equals_ignore_case(name, "Content-Location") &&
                !http_ascii_equals_ignore_case(name, "Last-Modified")) {
                cached_headers.header(
                    name, field.value(), {.mode_ = http_response_header_mode::append});
            } else {
                cached_headers.header(name, field.value());
            }
        } else {
            cached_headers.emplace_back(name, field.value());
        }
    }
}
}  // namespace

std::string rewrite_http_cache_control_max_age(
    std::string_view field_value, std::uint64_t client_max_age) {
    std::string output;
    append_preserved_cache_control_directives(field_value, output);
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

std::string detail::rewrite_http_cache_control_max_age_headers(
    detail::http_header_range_view headers, std::uint64_t client_max_age) {
    std::string output;
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

void rewrite_http_response_cache_max_age(
    std::vector<http_header>& response_headers, std::uint64_t client_max_age) {
    auto rewritten = rewrite_http_cache_control_max_age(response_headers, client_max_age);
    std::erase_if(response_headers, [](const http_header& field) noexcept {
        return http_ascii_equals_ignore_case(field.name(), "Cache-Control") ||
               http_ascii_equals_ignore_case(field.name(), "Expires");
    });
    response_headers.emplace_back("Cache-Control", rewritten);
}

void rewrite_http_response_cache_max_age(
    http_response_headers& response_headers, std::uint64_t client_max_age) {
    std::string output;
    for (const auto& field : response_headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    response_headers.remove_header("Expires");
    response_headers.header("Cache-Control", output);
}

void rewrite_http_response_cache_max_age(
    http_response& response, std::uint64_t client_max_age) {
    auto staged = response.headers().clone(1);
    rewrite_http_response_cache_max_age(staged, client_max_age);
    response.commit_headers(std::move(staged));
}

void http_vary_spec::update(std::string_view field_value) {
    if (wildcard_) {
        return;
    }
    detail::http_visit_comma_separated_tokens(field_value, [&](std::string_view token) {
        if (token == "*") {
            wildcard_ = true;
            field_names_.clear();
            return false;
        }
        if (!is_valid_http_header_name(token)) {
            return true;
        }
        std::string lower(token);
        for (auto& ch : lower) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        const auto it = std::ranges::lower_bound(field_names_, lower);
        if (it == field_names_.end() || *it != lower) {
            field_names_.insert(it, std::move(lower));
        }
        return true;
    });
}

void http_vary_spec::update_header_range(detail::http_header_range_view headers) {
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Vary")) {
            update(field.value());
        }
    }
}

http_vary_spec parse_http_vary(std::string_view field_value) {
    http_vary_spec spec;
    spec.update(field_value);
    return spec;
}

std::optional<std::string> detail::format_http_vary_cache_key_headers(
    const http_vary_spec& vary, detail::http_header_range_view request_headers) {
    if (vary.is_wildcard()) {
        return std::nullopt;
    }
    std::string key;
    for (const auto& field_name : vary.field_names()) {
        key.append(field_name);
        bool present = false;
        for (const auto header : request_headers) {
            if (!http_ascii_equals_ignore_case(header.name(), field_name)) {
                continue;
            }
            if (!present) {
                key.append(":1:");
                present = true;
            } else {
                key.append(", ");
            }
            key.append(normalize_vary_field_value(header.value()));
        }
        if (!present) {
            key.append(":0");
        }
        key.push_back('\n');
    }
    return key;
}

std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept {
    const auto trimmed = http_trim_ows(field_value);
    if (trimmed.empty() || trimmed.front() == '"') {
        return std::nullopt;
    }
    return parse_delta_seconds(trimmed);
}

std::uint64_t detail::compute_http_cache_age_headers(
    detail::http_header_range_view response_headers, std::time_t request_time,
    std::time_t response_time, std::time_t now) noexcept {
    std::optional<std::time_t> date_value;
    std::optional<std::uint64_t> age_value;
    for (const auto field : response_headers) {
        if (!date_value.has_value() && http_ascii_equals_ignore_case(field.name(), "Date")) {
            date_value = parse_http_date(field.value());
        } else if (!age_value.has_value() && http_ascii_equals_ignore_case(field.name(), "Age")) {
            age_value = parse_http_age(field.value());
        }
    }
    return compute_http_cache_age(date_value, age_value, request_time, response_time, now);
}

bool is_http_304_forbidden_header(std::string_view field_name) noexcept {
    switch (field_name.size()) {
        case 6U:
            return http_ascii_equals_ignore_case(field_name, "Digest");
        case 7U:
            return http_ascii_equals_ignore_case(field_name, "Trailer");
        case 11U:
            return http_ascii_equals_ignore_case(field_name, "Content-MD5") ||
                   http_ascii_equals_ignore_case(field_name, "Repr-Digest");
        case 12U:
            return http_ascii_equals_ignore_case(field_name, "Content-Type");
        case 13U:
            return http_ascii_equals_ignore_case(field_name, "Content-Range");
        case 14U:
            return http_ascii_equals_ignore_case(field_name, "Content-Length") ||
                   http_ascii_equals_ignore_case(field_name, "Content-Digest");
        case 16U:
            return http_ascii_equals_ignore_case(field_name, "Content-Encoding") ||
                   http_ascii_equals_ignore_case(field_name, "Content-Language");
        case 17U:
            return http_ascii_equals_ignore_case(field_name, "Transfer-Encoding");
        case 19U:
            return http_ascii_equals_ignore_case(field_name, "Content-Disposition");
        default:
            return false;
    }
}

void sanitize_http_304_response_headers(std::vector<http_header>& headers) {
    std::erase_if(headers, [](const http_header& field) noexcept {
        return is_http_304_forbidden_header(field.name());
    });
}

void sanitize_http_304_response_headers(http_response_headers& headers) {
    std::vector<std::string> to_remove;
    for (const auto& field : headers) {
        if (is_http_304_forbidden_header(field.name())) {
            to_remove.emplace_back(field.name());
        }
    }
    for (const auto& name : to_remove) {
        headers.remove_header(name);
    }
}

void sanitize_http_304_response_headers(http_response& response) {
    auto staged = response.headers().clone();
    sanitize_http_304_response_headers(staged);
    response.commit_headers(std::move(staged));
}

void detail::merge_http_304_response_headers_into(
    std::vector<http_header>& cached_headers, detail::http_header_range_view not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}

void detail::merge_http_304_response_headers_into(
    http_response_headers& cached_headers, detail::http_header_range_view not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}
void merge_http_304_response_headers(
    http_response& cached_response, const http_response& not_modified_response) {
    auto staged = cached_response.headers().clone(not_modified_response.headers().size());
    merge_http_304_response_headers(staged, not_modified_response.headers());
    cached_response.commit_headers(std::move(staged));
}

}  // namespace ruvia

namespace ruvia::detail {

// UTC calendar fields for an HTTP-date, or no value outside years 0000-9999
// (the four-digit IMF-fixdate year). Civil conversion is independent of the C
// runtime's date range and locale; in particular, Windows gmtime_s cannot
// represent pre-1970 UTC timestamps. The range check runs before calendar
// conversion so huge time_t values cannot overflow it.
[[nodiscard]] std::optional<std::tm> http_utc_tm(std::time_t time) noexcept {
    using namespace std::chrono;
    constexpr auto first = duration_cast<seconds>(sys_days{year{0} / January / 1}.time_since_epoch()).count();
    constexpr auto end = duration_cast<seconds>(sys_days{year{10000} / January / 1}.time_since_epoch()).count();
    if (std::cmp_less(time, first) || std::cmp_greater_equal(time, end)) {
        return std::nullopt;
    }
    const sys_seconds instant{seconds{static_cast<seconds::rep>(time)}};
    const auto date = floor<days>(instant);
    const year_month_day calendar{date};
    const hh_mm_ss clock{instant - date};
    std::tm utc{};
    utc.tm_year = static_cast<int>(calendar.year()) - 1900;
    utc.tm_mon = static_cast<int>(static_cast<unsigned>(calendar.month())) - 1;
    utc.tm_mday = static_cast<int>(static_cast<unsigned>(calendar.day()));
    utc.tm_wday = static_cast<int>(weekday{date}.c_encoding());
    utc.tm_hour = static_cast<int>(clock.hours().count());
    utc.tm_min = static_cast<int>(clock.minutes().count());
    utc.tm_sec = static_cast<int>(clock.seconds().count());
    return utc;
}

std::optional<std::time_t> http_civil_to_time_t(
    int year, int month, int day, int hour, int minute, int second) noexcept {
    if (year < static_cast<int>((std::chrono::year::min)()) || year > static_cast<int>((std::chrono::year::max)()) ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days_since_epoch = static_cast<std::int64_t>(std::chrono::sys_days{date}.time_since_epoch().count());
    const auto total = days_since_epoch * 86400 + static_cast<std::int64_t>(hour) * 3600 +
                       static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
    if (!std::in_range<std::time_t>(total)) {
        return std::nullopt;
    }
    return static_cast<std::time_t>(total);
}

namespace {

// "Date: <value>\r\n" -- the cache is the single owner of this wire format.
// Both the full-line accessor (HTTP/1 text response) and the
// value-only accessor (HPACK :date for HTTP/2) derive their views from these
// constants, so the framing lives in exactly one place.
inline constexpr std::string_view date_header_prefix = "Date: ";
inline constexpr std::string_view date_header_suffix = "\r\n";

struct date_cache final {
    std::array<char, 64> line_{};
    std::size_t size_{0};
    std::time_t second_{};
    bool initialized_{false};
};

[[nodiscard]] date_cache& worker_date_cache() noexcept {
    thread_local date_cache cache;
    return cache;
}

}  // namespace

std::string_view cached_date_header() noexcept {
    const auto now = std::time(nullptr);
    auto& cache = worker_date_cache();
    if (!cache.initialized_ || cache.second_ != now) {
        cache.second_ = now;
        cache.initialized_ = true;
        cache.size_ = 0;
        // RFC 9110 section 6.6.1: do not generate Date without a usable clock.
        if (now != std::time_t{-1}) {
            if (const auto date = format_http_date(now)) {
                std::memcpy(cache.line_.data(), date_header_prefix.data(), date_header_prefix.size());
                std::memcpy(cache.line_.data() + date_header_prefix.size(), date->data(), date->size());
                cache.size_ = date_header_prefix.size() + date->size();
                cache.line_[cache.size_++] = date_header_suffix[0];
                cache.line_[cache.size_++] = date_header_suffix[1];
            }
        }
    }
    return std::string_view(cache.line_.data(), cache.size_);
}

std::string_view cached_date_value() noexcept {
    const auto line = cached_date_header();
    constexpr std::size_t framing = date_header_prefix.size() + date_header_suffix.size();
    if (line.size() <= framing) {
        return {};
    }
    return line.substr(date_header_prefix.size(), line.size() - framing);
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

// IMF-fixdate day names (RFC 9110 section 5.6.7), indexed by tm_wday.
constexpr std::array<std::string_view, 7> http_weekday_names{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

[[nodiscard]] int month_index(std::string_view value) noexcept {
    for (std::size_t i = 0; i < detail::http_month_names.size(); ++i) {
        if (value == detail::http_month_names[i]) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

[[nodiscard]] bool is_short_weekday(std::string_view value) noexcept {
    return std::ranges::find(http_weekday_names, value) != http_weekday_names.end();
}

[[nodiscard]] bool is_long_weekday(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 7> weekdays{
        "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    return std::ranges::find(weekdays, value) != weekdays.end();
}

[[nodiscard]] std::optional<int> parse_fixed_digits(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    int parsed_value = 0;
    for (const auto c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        parsed_value = parsed_value * 10 + (c - '0');
    }
    return parsed_value;
}

[[nodiscard]] std::optional<std::time_t> parse_imf_fixdate(std::string_view value) noexcept {
    if (value.size() != http_imf_fixdate_size || !is_short_weekday(value.substr(0, 3)) || value[3] != ',' ||
        value[4] != ' ' || value[7] != ' ' || value[11] != ' ' || value[16] != ' ' ||
        value[19] != ':' || value[22] != ':' || value[25] != ' ' || value.substr(26, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = parse_fixed_digits(value.substr(5, 2));
    const auto month = month_index(value.substr(8, 3));
    const auto year = parse_fixed_digits(value.substr(12, 4));
    const auto hour = parse_fixed_digits(value.substr(17, 2));
    const auto minute = parse_fixed_digits(value.substr(20, 2));
    const auto second = parse_fixed_digits(value.substr(23, 2));
    if (!day || month == 0 || !year || !hour || !minute || !second) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] std::optional<std::time_t> parse_asctime_date(std::string_view value) noexcept {
    if (value.size() != 24 || value[3] != ' ' || value[7] != ' ' || value[10] != ' ' ||
        value[13] != ':' || value[16] != ':' || value[19] != ' ' ||
        !is_short_weekday(value.substr(0, 3))) {
        return std::nullopt;
    }
    const auto month = month_index(value.substr(4, 3));
    auto day_field = value.substr(8, 2);
    if (day_field.front() == ' ') {
        day_field.remove_prefix(1);
    }
    const auto day = parse_fixed_digits(day_field);
    const auto hour = parse_fixed_digits(value.substr(11, 2));
    const auto minute = parse_fixed_digits(value.substr(14, 2));
    const auto second = parse_fixed_digits(value.substr(17, 2));
    const auto year = parse_fixed_digits(value.substr(20, 4));
    if (month == 0 || !day || !hour || !minute || !second || !year) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] int resolve_rfc850_year(int short_year, int current_year) noexcept {
    const auto current_century = current_year - current_year % 100;
    auto year = current_century + short_year;
    // RFC 9110 section 5.6.7: an obsolete two-digit date that appears more
    // than 50 years in the future denotes the most recent past year with the
    // same final two digits. This is a rolling pivot, not the fixed POSIX
    // 68/69 split.
    if (year > current_year + 50) {
        year -= 100;
    }
    return year;
}

[[nodiscard]] std::optional<std::time_t> parse_rfc850_date(std::string_view value) noexcept {
    const auto comma = value.find(", ");
    if (comma == std::string_view::npos || !is_long_weekday(value.substr(0, comma))) {
        return std::nullopt;
    }
    const auto body = value.substr(comma + 2);
    if (body.size() != 22 || body[2] != '-' || body[6] != '-' || body[9] != ' ' ||
        body[12] != ':' || body[15] != ':' || body[18] != ' ' || body.substr(19, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = parse_fixed_digits(body.substr(0, 2));
    const auto month = month_index(body.substr(3, 3));
    const auto short_year = parse_fixed_digits(body.substr(7, 2));
    const auto hour = parse_fixed_digits(body.substr(10, 2));
    const auto minute = parse_fixed_digits(body.substr(13, 2));
    const auto second = parse_fixed_digits(body.substr(16, 2));
    if (!day || month == 0 || !short_year || !hour || !minute || !second) {
        return std::nullopt;
    }
    const auto now = std::time(nullptr);
    if (now == std::time_t{-1}) {
        return std::nullopt;
    }
    const auto current_utc = detail::http_utc_tm(now);
    if (!current_utc) {
        return std::nullopt;
    }
    const int current_year = current_utc->tm_year + 1900;
    auto year = resolve_rfc850_year(*short_year, current_year);
    // RFC 9110 section 5.6.7 applies the rolling 50-year pivot to the full
    // timestamp. Equal years alone do not establish that a date is in range.
    const std::array date_fields{month, *day, *hour, *minute, *second};
    const std::array reference_fields{current_utc->tm_mon + 1, current_utc->tm_mday, current_utc->tm_hour,
        current_utc->tm_min, current_utc->tm_sec};
    if (year == current_year + 50 && date_fields > reference_fields) {
        year -= 100;
    }
    return detail::http_civil_to_time_t(year, month, *day, *hour, *minute, *second);
}

}  // namespace

// Allocation-free wire value; an unrepresentable date must not be truncated or
// substituted with a different timestamp. Fixed English names are independent
// of the process locale, as required by RFC 9110 section 5.6.7.
std::optional<std::array<char, http_imf_fixdate_size>> format_http_date(std::time_t value) noexcept {
    const auto utc = detail::http_utc_tm(value);
    if (!utc) {
        return std::nullopt;
    }
    std::array<char, http_imf_fixdate_size> output{};
    auto* out = output.data();
    const long year = static_cast<long>(utc->tm_year) + 1900;

    std::size_t i = 0;
    const auto put2 = [out, &i](int field) noexcept {
        out[i++] = static_cast<char>('0' + (field / 10) % 10);
        out[i++] = static_cast<char>('0' + field % 10);
    };
    const auto put3 = [out, &i](std::string_view name) noexcept {
        out[i++] = name[0];
        out[i++] = name[1];
        out[i++] = name[2];
    };

    put3(http_weekday_names[static_cast<std::size_t>(utc->tm_wday)]);
    out[i++] = ',';
    out[i++] = ' ';
    put2(utc->tm_mday);
    out[i++] = ' ';
    put3(detail::http_month_names[static_cast<std::size_t>(utc->tm_mon)]);
    out[i++] = ' ';
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 1000) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 100) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 10) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>(year % 10));
    out[i++] = ' ';
    put2(utc->tm_hour);
    out[i++] = ':';
    put2(utc->tm_min);
    out[i++] = ':';
    put2(utc->tm_sec);
    out[i++] = ' ';
    out[i++] = 'G';
    out[i++] = 'M';
    out[i++] = 'T';
    return output;
}

std::optional<std::time_t> parse_http_date(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (const auto imf = parse_imf_fixdate(value)) {
        return imf;
    }
    if (const auto rfc850 = parse_rfc850_date(value)) {
        return rfc850;
    }
    return parse_asctime_date(value);
}

}  // namespace ruvia

namespace ruvia {
namespace {

constexpr std::string_view v1_signature = "PROXY ";
// Spec §2.1: the longest line, "PROXY UNKNOWN" with two IPv6 addresses and
// two ports, including CRLF.
constexpr std::size_t v1_max_line_bytes = 107;
constexpr std::array<std::uint8_t, 12> v2_signature{
    0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A};
constexpr std::size_t v2_fixed_bytes = 16;
constexpr std::size_t v2_inet_block_bytes = 12;
constexpr std::size_t v2_inet6_block_bytes = 36;
constexpr std::size_t v2_unix_block_bytes = 216;
constexpr std::size_t v2_tlv_header_bytes = 3;

constexpr std::uint8_t af_unspec = 0x0;
constexpr std::uint8_t af_inet = 0x1;
constexpr std::uint8_t af_inet6 = 0x2;
constexpr std::uint8_t af_unix = 0x3;

[[noreturn]] void reject(const char* reason) {
    throw proxy_protocol_error(reason);
}

[[nodiscard]] constexpr std::uint8_t byte_value(std::byte value) noexcept {
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] constexpr std::uint16_t load_big_endian_16(std::span<const std::byte> bytes) noexcept {
    return static_cast<std::uint16_t>((byte_value(bytes[0]) << 8) | byte_value(bytes[1]));
}

// Spec §2.2: the address block that precedes TLVs; AF_UNSPEC has none whose
// size is known.
[[nodiscard]] constexpr std::size_t v2_address_block_bytes(std::uint8_t family) noexcept {
    switch (family) {
        case af_inet:
            return v2_inet_block_bytes;
        case af_inet6:
            return v2_inet6_block_bytes;
        case af_unix:
            return v2_unix_block_bytes;
        default:
            return 0;
    }
}

[[nodiscard]] std::string_view as_text(std::span<const std::byte> bytes) noexcept {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Spec §2.1: canonical address text of the announced family, without a zone.
[[nodiscard]] std::optional<ip_address> parse_v1_address(std::string_view text, ip_address_family family) noexcept {
    if (text.find('%') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto address = ip_address::parse(text);
    if (!address || address->family() != family) {
        return std::nullopt;
    }
    return address;
}

// Spec §2.1: decimal 0..65535 without heading zeroes.
[[nodiscard]] std::optional<std::uint16_t> parse_v1_port(std::string_view text) noexcept {
    if (text.empty() || text.size() > 5 || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(digit - '0');
    }
    if (value > 0xFFFF) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

// Splits "<src> <dst> <sport> <dport>" at single spaces; an empty field or a
// field count other than four rejects.
[[nodiscard]] std::optional<std::array<std::string_view, 4>> split_v1_fields(std::string_view text) noexcept {
    std::array<std::string_view, 4> fields;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto space = text.find(' ');
        const bool last = i + 1 == fields.size();
        if (last != (space == std::string_view::npos)) {
            return std::nullopt;
        }
        fields[i] = text.substr(0, space);
        if (fields[i].empty()) {
            return std::nullopt;
        }
        if (!last) {
            text.remove_prefix(space + 1);
        }
    }
    return fields;
}

[[nodiscard]] ip_endpoint load_v2_endpoint(
    std::span<const std::byte> address, std::span<const std::byte> port, ip_address_family family) noexcept {
    if (family == ip_address_family::ipv4) {
        std::array<std::uint8_t, 4> bytes{};
        std::transform(address.begin(), address.end(), bytes.begin(), byte_value);
        return ip_endpoint(ip_address::ipv4(bytes), load_big_endian_16(port));
    }
    std::array<std::uint8_t, 16> bytes{};
    std::transform(address.begin(), address.end(), bytes.begin(), byte_value);
    return ip_endpoint(ip_address::ipv6(bytes), load_big_endian_16(port));
}

}  // namespace

proxy_protocol_parser::proxy_protocol_parser(proxy_protocol_parser_options options)
    : max_v2_header_bytes_(options.max_v2_header_bytes_) {
    if (max_v2_header_bytes_ < v2_fixed_bytes + v2_inet6_block_bytes) {
        throw std::invalid_argument("PROXY protocol v2 header limit cannot hold an AF_INET6 address block");
    }
}

proxy_protocol_parse_result proxy_protocol_parser::parse(std::span<const std::byte> input) & {
    if (state_ != proxy_protocol_parser_state::active) {
        throw std::logic_error("PROXY protocol parser is not active");
    }
    try {
        return parse_bytes(input);
    } catch (...) {
        // Fail closed: the caller cannot know how much of a rejected or
        // unbufferable input was taken.
        state_ = proxy_protocol_parser_state::failed;
        throw;
    }
}

const proxy_protocol_header& proxy_protocol_parser::header() const& {
    if (state_ != proxy_protocol_parser_state::finished) {
        throw std::logic_error("PROXY protocol header is not complete");
    }
    return header_;
}

void proxy_protocol_parser::reset() noexcept {
    buffer_.clear();
    tlvs_.clear();
    header_ = proxy_protocol_header();
    v2_header_size_ = 0;
    v2_family_ = 0;
    phase_ = parse_phase::signature;
    state_ = proxy_protocol_parser_state::active;
}

proxy_protocol_parse_result proxy_protocol_parser::parse_bytes(std::span<const std::byte> input) {
    std::size_t position = 0;
    while (position < input.size() && state_ == proxy_protocol_parser_state::active) {
        switch (phase_) {
            case parse_phase::signature:
                position = consume_signature(input, position);
                break;
            case parse_phase::v1_line:
                position = consume_v1_line(input, position);
                break;
            case parse_phase::v2_fixed:
                position = consume_v2_fixed(input, position);
                break;
            case parse_phase::v2_block:
                position = consume_v2_block(input, position);
                break;
        }
    }
    return {.complete_ = state_ == proxy_protocol_parser_state::finished, .consumed_bytes_ = position};
}

// The first byte selects the only signature it can begin ('P' or CR), so each
// later byte either continues that signature or rules out both.
std::size_t proxy_protocol_parser::consume_signature(std::span<const std::byte> input, std::size_t position) {
    const auto value = byte_value(input[position]);
    const auto index = buffer_.size();
    if (index == 0) {
        if (value == static_cast<std::uint8_t>(v1_signature.front())) {
            header_.version_ = proxy_protocol_version::v1;
        } else if (value == v2_signature.front()) {
            header_.version_ = proxy_protocol_version::v2;
        } else {
            reject("input does not begin a PROXY protocol signature");
        }
    } else {
        const auto expected = header_.version_ == proxy_protocol_version::v1
                                  ? static_cast<std::uint8_t>(v1_signature[index])
                                  : v2_signature[index];
        if (value != expected) {
            reject("input does not begin a PROXY protocol signature");
        }
    }
    buffer_.push_back(input[position]);
    if (header_.version_ == proxy_protocol_version::v1 && buffer_.size() == v1_signature.size()) {
        phase_ = parse_phase::v1_line;
    } else if (header_.version_ == proxy_protocol_version::v2 && buffer_.size() == v2_signature.size()) {
        phase_ = parse_phase::v2_fixed;
    }
    return position + 1;
}

// Spec §2.1: the line ends at the first CRLF within 107 bytes; a lone CR or LF
// rejects. Bytes before the end are only searched for CR and LF here; the
// fields are decoded once, after the CRLF arrives.
std::size_t proxy_protocol_parser::consume_v1_line(std::span<const std::byte> input, std::size_t position) {
    // The signature ends in a space, so a buffered trailing CR is a line's CR
    // whose LF did not arrive in the previous input.
    if (buffer_.back() == std::byte{'\r'}) {
        if (input[position] != std::byte{'\n'}) {
            reject("PROXY protocol v1 CR is not followed by LF");
        }
        buffer_.push_back(input[position]);
        finish_v1();
        return position + 1;
    }
    const auto chunk = input.subspan(position, std::min(input.size() - position, v1_max_line_bytes - buffer_.size()));
    const auto stop = std::find_if(chunk.begin(), chunk.end(),
        [](std::byte value) { return value == std::byte{'\r'} || value == std::byte{'\n'}; });
    if (stop != chunk.end() && *stop == std::byte{'\n'}) {
        reject("PROXY protocol v1 line ends in a bare LF");
    }
    const auto taken = static_cast<std::size_t>(stop - chunk.begin()) + (stop == chunk.end() ? 0 : 1);
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(taken));
    // Full without CRLF, or a CR that leaves no room for its LF.
    if (buffer_.size() >= v1_max_line_bytes) {
        reject("PROXY protocol v1 line exceeds 107 bytes");
    }
    return position + taken;
}

std::size_t proxy_protocol_parser::consume_v2_fixed(std::span<const std::byte> input, std::size_t position) {
    const auto value = byte_value(input[position]);
    switch (buffer_.size()) {
        case 12:
            // Spec §2.2: the receiver must only accept version 2 and must drop
            // unassigned commands.
            if ((value >> 4) != 0x2) {
                reject("PROXY protocol v2 version is not 2");
            }
            switch (value & 0x0F) {
                case 0x0:
                    header_.command_ = proxy_protocol_command::local;
                    break;
                case 0x1:
                    header_.command_ = proxy_protocol_command::proxy;
                    break;
                default:
                    reject("PROXY protocol v2 command is unassigned");
            }
            break;
        case 13:
            v2_family_ = static_cast<std::uint8_t>(value >> 4);
            if (v2_family_ > af_unix) {
                reject("PROXY protocol v2 address family is unspecified");
            }
            switch (value & 0x0F) {
                case 0x0:
                    header_.transport_ = proxy_protocol_transport::unspecified;
                    break;
                case 0x1:
                    header_.transport_ = proxy_protocol_transport::stream;
                    break;
                case 0x2:
                    header_.transport_ = proxy_protocol_transport::datagram;
                    break;
                default:
                    reject("PROXY protocol v2 transport protocol is unspecified");
            }
            break;
        default:
            break;
    }
    buffer_.push_back(input[position]);
    if (buffer_.size() < v2_fixed_bytes) {
        return position + 1;
    }
    const std::size_t length = load_big_endian_16(std::span<const std::byte>(buffer_).subspan(14, 2));
    if (length > max_v2_header_bytes_ - v2_fixed_bytes) {
        reject("PROXY protocol v2 header exceeds the configured limit");
    }
    if (header_.command_ == proxy_protocol_command::proxy && length < v2_address_block_bytes(v2_family_)) {
        reject("PROXY protocol v2 length does not cover the address block");
    }
    v2_header_size_ = v2_fixed_bytes + length;
    phase_ = parse_phase::v2_block;
    if (length == 0) {
        finish_v2();
    }
    return position + 1;
}

// Spec §2.2: the receiver must not parse an address before the whole block
// has arrived.
std::size_t proxy_protocol_parser::consume_v2_block(std::span<const std::byte> input, std::size_t position) {
    const auto taken = std::min(input.size() - position, v2_header_size_ - buffer_.size());
    const auto chunk = input.subspan(position, taken);
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    if (buffer_.size() == v2_header_size_) {
        finish_v2();
    }
    return position + taken;
}

void proxy_protocol_parser::finish_v1() {
    // Without the signature and the CRLF.
    const auto line = as_text(std::span<const std::byte>(buffer_)).substr(v1_signature.size(), buffer_.size() - v1_signature.size() - 2);
    header_.command_ = proxy_protocol_command::proxy;
    constexpr std::string_view unknown = "UNKNOWN";
    if (line.starts_with(unknown) && (line.size() == unknown.size() || line[unknown.size()] == ' ')) {
        // Spec §2.1: anything after UNKNOWN is ignored and the real endpoints
        // are used.
        header_.transport_ = proxy_protocol_transport::unspecified;
        state_ = proxy_protocol_parser_state::finished;
        return;
    }
    const bool tcp4 = line.starts_with("TCP4 ");
    if (!tcp4 && !line.starts_with("TCP6 ")) {
        reject("PROXY protocol v1 protocol is not TCP4, TCP6 or UNKNOWN");
    }
    const auto family = tcp4 ? ip_address_family::ipv4 : ip_address_family::ipv6;
    const auto fields = split_v1_fields(line.substr(5));
    if (!fields) {
        reject("PROXY protocol v1 line does not have four single-space separated fields");
    }
    const auto source_address = parse_v1_address((*fields)[0], family);
    const auto destination_address = parse_v1_address((*fields)[1], family);
    if (!source_address || !destination_address) {
        reject("PROXY protocol v1 address is not canonical text of the announced family");
    }
    const auto source_port = parse_v1_port((*fields)[2]);
    const auto destination_port = parse_v1_port((*fields)[3]);
    if (!source_port || !destination_port) {
        reject("PROXY protocol v1 port is not a decimal 0..65535 without heading zeroes");
    }
    header_.transport_ = proxy_protocol_transport::stream;
    header_.source_ = ip_endpoint(*source_address, *source_port);
    header_.destination_ = ip_endpoint(*destination_address, *destination_port);
    state_ = proxy_protocol_parser_state::finished;
}

void proxy_protocol_parser::finish_v2() {
    // Spec §2.2: LOCAL discards the block, family included. With AF_UNSPEC the
    // address length is unknown, so no TLV boundary can be located.
    if (header_.command_ == proxy_protocol_command::local || v2_family_ == af_unspec) {
        state_ = proxy_protocol_parser_state::finished;
        return;
    }
    const auto block = std::span<const std::byte>(buffer_).subspan(v2_fixed_bytes);
    if (header_.transport_ != proxy_protocol_transport::unspecified && v2_family_ != af_unix) {
        const auto family = v2_family_ == af_inet ? ip_address_family::ipv4 : ip_address_family::ipv6;
        const std::size_t address_bytes = family == ip_address_family::ipv4 ? 4 : 16;
        const auto ports = block.subspan(2 * address_bytes, 4);
        header_.source_ = load_v2_endpoint(block.subspan(0, address_bytes), ports.subspan(0, 2), family);
        header_.destination_ = load_v2_endpoint(block.subspan(address_bytes, address_bytes), ports.subspan(2, 2), family);
    }
    // Spec §2.2.3: PP2_TYPE_CRC32C verification is a MAY; every TLV, that one
    // included, is exposed raw.
    auto rest = block.subspan(v2_address_block_bytes(v2_family_));
    while (!rest.empty()) {
        if (rest.size() < v2_tlv_header_bytes) {
            reject("PROXY protocol v2 TLV header overruns the header");
        }
        const std::size_t value_bytes = load_big_endian_16(rest.subspan(1, 2));
        if (value_bytes > rest.size() - v2_tlv_header_bytes) {
            reject("PROXY protocol v2 TLV value overruns the header");
        }
        tlvs_.push_back({.type_ = byte_value(rest[0]), .value_ = rest.subspan(v2_tlv_header_bytes, value_bytes)});
        rest = rest.subspan(v2_tlv_header_bytes + value_bytes);
    }
    header_.tlvs_ = tlvs_;
    state_ = proxy_protocol_parser_state::finished;
}

}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] std::optional<std::uint32_t> parse_zone(std::string_view text) noexcept {
    if (text.empty() || text.front() == '0') {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] char* append_ipv4(char* cursor, char* end, const std::uint8_t* bytes) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        if (i != 0) {
            *cursor++ = '.';
        }
        cursor = std::to_chars(cursor, end, bytes[i]).ptr;
    }
    return cursor;
}

}  // namespace

std::optional<ip_address> ip_address::parse(std::string_view text) noexcept {
    if (text.find(':') == std::string_view::npos) {
        const auto octets = detail::parse_ipv4_address(text);
        if (!octets) {
            return std::nullopt;
        }
        return ip_address::ipv4(*octets);
    }
    std::uint32_t scope_id = 0;
    const auto zone = text.find('%');
    if (zone != std::string_view::npos) {
        const auto parsed_zone = parse_zone(text.substr(zone + 1));
        if (!parsed_zone) {
            return std::nullopt;
        }
        scope_id = *parsed_zone;
        text = text.substr(0, zone);
    }
    const auto groups = detail::parse_ipv6_address(text);
    if (!groups) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < groups->size(); ++i) {
        bytes[2 * i] = static_cast<std::uint8_t>((*groups)[i] >> 8);
        bytes[2 * i + 1] = static_cast<std::uint8_t>((*groups)[i] & 0xffU);
    }
    return ip_address::ipv6(bytes, scope_id);
}

std::string ip_address::to_string() const {
    // "ffff:" * 7 + "ffff" + "%" + 10 zone digits.
    std::array<char, 56> buffer;
    char* const end = buffer.data() + buffer.size();
    char* cursor = buffer.data();
    if (family_ == ip_address_family::ipv4) {
        cursor = append_ipv4(cursor, end, bytes_.data());
    } else {
        bool mapped = bytes_[10] == 0xffU && bytes_[11] == 0xffU;
        for (std::size_t i = 0; mapped && i < 10; ++i) {
            mapped = bytes_[i] == 0;
        }
        if (mapped) {
            const std::string_view prefix = "::ffff:";
            cursor = std::copy(prefix.begin(), prefix.end(), cursor);
            cursor = append_ipv4(cursor, end, bytes_.data() + 12);
        } else {
            std::array<std::uint16_t, 8> groups{};
            for (std::size_t i = 0; i < groups.size(); ++i) {
                groups[i] = static_cast<std::uint16_t>(bytes_[2 * i] << 8 | bytes_[2 * i + 1]);
            }
            // RFC 5952 §4.2: compress the first longest run of two or more
            // zero groups.
            std::size_t best_begin = groups.size();
            std::size_t best_size = 1;
            for (std::size_t i = 0; i < groups.size();) {
                if (groups[i] != 0) {
                    ++i;
                    continue;
                }
                std::size_t run_end = i;
                while (run_end < groups.size() && groups[run_end] == 0) {
                    ++run_end;
                }
                if (run_end - i > best_size) {
                    best_begin = i;
                    best_size = run_end - i;
                }
                i = run_end;
            }
            for (std::size_t i = 0; i < groups.size();) {
                if (i == best_begin) {
                    *cursor++ = ':';
                    *cursor++ = ':';
                    i += best_size;
                    continue;
                }
                if (i != 0 && i != best_begin + best_size) {
                    *cursor++ = ':';
                }
                cursor = std::to_chars(cursor, end, groups[i], 16).ptr;
                ++i;
            }
        }
        if (scope_id_ != 0) {
            *cursor++ = '%';
            cursor = std::to_chars(cursor, end, scope_id_).ptr;
        }
    }
    return std::string(buffer.data(), static_cast<std::size_t>(cursor - buffer.data()));
}

namespace {

[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> to_mapped_ipv6_bytes(
    const ip_address& address) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        const auto v4 = address.bytes();
        return std::array<std::uint8_t, 16>{
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, v4[0], v4[1], v4[2], v4[3]};
    }
    // Scoped IPv6 literals compare by their 128-bit address prefix in CIDR blocks.
    std::array<std::uint8_t, 16> v6{};
    const auto raw = address.bytes();
    std::copy_n(raw.data(), 16, v6.data());
    return v6;
}

}  // namespace

std::optional<ip_cidr> ip_cidr::parse(std::string_view text) noexcept {
    const auto slash = text.find('/');
    const auto address_text = slash == std::string_view::npos ? text : text.substr(0, slash);
    const auto parsed = ip_address::parse(address_text);
    if (!parsed) {
        return std::nullopt;
    }
    const auto mapped = to_mapped_ipv6_bytes(*parsed);
    if (!mapped) {
        return std::nullopt;
    }
    const bool was_v4 = parsed->family() == ip_address_family::ipv4;
    const unsigned max_bits = was_v4 ? 32U : 128U;
    unsigned bits = max_bits;
    if (slash != std::string_view::npos) {
        const auto prefix_text = text.substr(slash + 1);
        if (prefix_text.empty() || prefix_text.size() > 3) {
            return std::nullopt;
        }
        const auto [end, err] =
            std::from_chars(prefix_text.data(), prefix_text.data() + prefix_text.size(), bits);
        if (err != std::errc{} || end != prefix_text.data() + prefix_text.size() ||
            bits > max_bits) {
            return std::nullopt;
        }
    }
    ip_cidr result;
    result.network_ = *mapped;
    result.prefix_bits_ = static_cast<std::uint8_t>(was_v4 ? bits + 96U : bits);
    return result;
}

bool ip_cidr::contains(const ip_address& address) const noexcept {
    const auto peer = to_mapped_ipv6_bytes(address);
    if (!peer) {
        return false;
    }
    const std::size_t full_bytes = prefix_bits_ / 8U;
    const std::uint8_t rem_bits = prefix_bits_ % 8U;
    if (!std::equal(network_.begin(), network_.begin() + full_bytes, peer->begin())) {
        return false;
    }
    if (rem_bits == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xffU << (8U - rem_bits));
    return (network_[full_bytes] & mask) == ((*peer)[full_bytes] & mask);
}

bool ip_cidr::contains(std::string_view address_text) const noexcept {
    const auto parsed = ip_address::parse(address_text);
    return parsed.has_value() && contains(*parsed);
}

}  // namespace ruvia

namespace ruvia {

http_connect_udp_target::http_connect_udp_target(std::string host, port_number port) noexcept
    : host_(std::move(host)),
      port_(port.value()) {}

namespace {
constexpr std::string_view prefix{"/.well-known/masque/udp/"};
bool valid_host(std::string_view host) {
    return !host.empty() && host.find('%') == std::string_view::npos &&
           (host.find(':') != std::string_view::npos ? detail::parse_ipv6_address(host).has_value() : detail::is_valid_reg_name(host));
}
// RFC 9297 section 3.2: these statuses cannot start the Capsule Protocol.
constexpr bool forbids_capsule_protocol(std::uint16_t status) noexcept {
    return status == 204 || status == 205 || status == 206;
}
[[nodiscard]] bool is_http_or_https_scheme(std::string_view scheme) noexcept {
    return http_ascii_equals_ignore_case(scheme, "http") ||
           http_ascii_equals_ignore_case(scheme, "https");
}
enum class handshake_message_role : std::uint8_t {
    request_recipient,
    request_sender,
    response_recipient,
    response_sender,
};
enum class capsule_protocol_state : std::uint8_t {
    absent,
    valid_true,
    invalid,
};
struct handshake_fields {
    capsule_protocol_state capsule_protocol_{capsule_protocol_state::absent};
    bool upgrade_{false}, upgrade_field_{false}, connection_{false}, connection_field_{false};
    bool host_{false}, forbidden_{false}, http2_forbidden_{false};
    std::string_view host_value_{};
};
template <typename header_type>
std::variant<handshake_fields, http_connect_udp_error> fields(std::span<const header_type> headers,
    handshake_message_role role, http_connect_udp_error syntax_error) noexcept {
    const auto connection_role =
        role == handshake_message_role::request_recipient
            ? detail::http_connection_field_role::request_recipient
            : (role == handshake_message_role::response_recipient
                      ? detail::http_connection_field_role::response_recipient
                      : detail::http_connection_field_role::sender);
    const auto list_role =
        (role == handshake_message_role::request_recipient ||
            role == handshake_message_role::response_recipient)
            ? detail::http_field_list_role::recipient
            : detail::http_field_list_role::sender;
    handshake_fields found;
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    bool connect_udp_offered = false;
    for (const auto& field : headers) {
        if (!is_valid_http_header_name(field.name()) || !is_valid_http_header_value(field.value())) {
            return syntax_error;
        }
        if (detail::is_forbidden_http_binary_connection_field(field.name())) {
            found.http2_forbidden_ = true;
        } else if (http_ascii_equals_ignore_case(field.name(), "te")) {
            if (role == handshake_message_role::response_recipient ||
                role == handshake_message_role::response_sender ||
                !detail::is_valid_binary_http_te_field_value(field.value(), list_role)) {
                found.http2_forbidden_ = true;
            }
        }
        if (http_ascii_equals_ignore_case(field.name(), "capsule-protocol")) {
            // RFC 9297 section 3.4: recipients handle a non-Boolean value as if the
            // field were absent; ?0 has the same meaning as absence. Only senders
            // require a single ?1 Capsule-Protocol field.
            if (list_role == detail::http_field_list_role::sender) {
                const auto value = parse_http_capsule_protocol(field.value());
                found.capsule_protocol_ =
                    (found.capsule_protocol_ == capsule_protocol_state::absent &&
                        value == std::optional<bool>(true))
                        ? capsule_protocol_state::valid_true
                        : capsule_protocol_state::invalid;
            }
        } else if (http_ascii_equals_ignore_case(field.name(), "upgrade")) {
            found.upgrade_field_ = true;
            if (upgrade_protocols.parse_field(field.value(), list_role,
                    [&](const detail::http_upgrade_protocol& protocol) noexcept {
                        const bool is_connect_udp =
                            protocol.version_.empty() &&
                            http_ascii_equals_ignore_case(protocol.name_, "connect-udp");
                        if (role == handshake_message_role::request_recipient) {
                            if (is_connect_udp) {
                                connect_udp_offered = true;
                            }
                            return true;
                        }
                        if (!is_connect_udp ||
                            (role != handshake_message_role::response_recipient && connect_udp_offered)) {
                            return false;
                        }
                        connect_udp_offered = true;
                        return true;
                    }) != detail::http_field_list_parse_status::ok) {
                return syntax_error;
            }
        } else if (http_ascii_equals_ignore_case(field.name(), "connection")) {
            found.connection_field_ = true;
            if (connection_options.parse_field(field.value(), connection_role) !=
                detail::http_field_list_parse_status::ok) {
                return syntax_error;
            }
            found.connection_ = connection_options.contains(detail::http_connection_option::upgrade);
        } else if (http_ascii_equals_ignore_case(field.name(), "host")) {
            const auto trimmed_host = http_trim_ows(field.value());
            if (found.host_ || !parse_http_authority(trimmed_host).has_value()) {
                return syntax_error;
            }
            found.host_ = true;
            found.host_value_ = trimmed_host;
        } else if (detail::http_field_name_in(field.name(), detail::http_connect_forbidden_fields)) {
            // RFC 9297 Section 3.2: the Capsule Protocol MUST NOT be used with
            // messages carrying Content-Length, Content-Type, or Transfer-Encoding
            // (the list also covers Content-Encoding and Trailer); a receiver that
            // observes one treats the message as malformed.
            found.forbidden_ = true;
        }
    }
    found.upgrade_ = upgrade_protocols.has_protocol() && connect_udp_offered;
    if (found.upgrade_field_ && !found.upgrade_) {
        return syntax_error;
    }
    return found;
}
template <typename header_type>
std::optional<http_connect_udp_error> validate_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const header_type> headers) noexcept {
    if (version == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_response;
    }
    const bool extended_connect = version != http_protocol_version::http11;
    const auto parsed_value =
        fields(headers, handshake_message_role::response_recipient, http_connect_udp_error::invalid_response);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    // The connect-udp token, not Capsule-Protocol, selects the Capsule Protocol
    // (RFC 9298 section 3); RFC 9297 section 3.2 makes 204/205/206 malformed.
    if (found.forbidden_ ||
        (extended_connect
                ? status < 200 || status >= 300 || forbids_capsule_protocol(status) || found.upgrade_field_ ||
                      found.connection_field_ || found.http2_forbidden_
                : status != 101 || !found.upgrade_ || !found.connection_)) {
        return http_connect_udp_error::invalid_response;
    }
    return std::nullopt;
}
}  // namespace
std::variant<std::string, http_connect_udp_error> encode_http_connect_udp_path(http_connect_udp_target_view target) {
    if (!valid_host(target.host_) || target.port_ == 0) {
        return http_connect_udp_error::invalid_target;
    }
    std::size_t encoded_host_size = 0;
    for (const unsigned char c : target.host_.view()) {
        encoded_host_size += detail::is_unreserved_byte(c) ? 1U : 3U;
    }
    std::array<char, http_max_unsigned_decimal_size> port_buffer{};
    const auto port_digits = http_format_unsigned(port_buffer, target.port_.value());
    std::string result_value;
    result_value.reserve(prefix.size() + encoded_host_size + 1U + port_digits.size() + 1U);
    result_value.append(prefix);
    for (const unsigned char c : target.host_.view()) {
        if (detail::is_unreserved_byte(c)) {
            result_value.push_back(static_cast<char>(c));
        } else {
            http_append_percent_byte(result_value, c);
        }
    }
    result_value.push_back('/');
    result_value.append(port_digits);
    result_value.push_back('/');
    return result_value;
}
std::variant<http_connect_udp_target, http_connect_udp_error> parse_http_connect_udp_path(std::string_view path) {
    if (!path.starts_with(prefix) || path.size() <= prefix.size() || !path.ends_with('/')) {
        return http_connect_udp_error::invalid_path;
    }
    path.remove_prefix(prefix.size());
    path.remove_suffix(1);
    const auto slash = path.find('/');
    if (slash == std::string_view::npos || path.find('/', slash + 1) != std::string_view::npos) {
        return http_connect_udp_error::invalid_path;
    }
    const auto raw_host = path.substr(0, slash);
    // Colons in IPv6 addresses must be escaped in this URI-template variable.
    if (raw_host.find(':') != std::string_view::npos) {
        return http_connect_udp_error::invalid_target;
    }
    const auto port = detail::parse_port_value(path.substr(slash + 1));
    if ((port.index() != 0) || !std::get<0>(port)) {
        return http_connect_udp_error::invalid_target;
    }
    std::string host;
    const auto decoded_host = decode_url_component(raw_host, host);
    if (!decoded_host || !valid_host(*decoded_host)) {
        return http_connect_udp_error::invalid_target;
    }
    if (decoded_host->data() == raw_host.data()) {
        host.assign(raw_host);
    }
    return http_connect_udp_target{std::move(host), std::get<0>(port)};
}
namespace {
std::optional<http_connect_udp_error> validate_request(http_connect_udp_request_view request, bool sending) noexcept {
    const auto authority = parse_http_authority(request.authority_);
    if (!authority || authority->host().empty() || !is_valid_http_origin_form_target(request.path_)) {
        return http_connect_udp_error::invalid_request;
    }
    const auto parsed_value = fields(request.headers_.view(),
        sending ? handshake_message_role::request_sender : handshake_message_role::request_recipient,
        http_connect_udp_error::invalid_request);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    if (sending && found.capsule_protocol_ != capsule_protocol_state::valid_true) {
        return http_connect_udp_error::invalid_capsule_protocol;
    }
    if (found.forbidden_ || request.authority_.empty() || request.path_.empty()) {
        return http_connect_udp_error::invalid_request;
    }
    if (request.version_ == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_request;
    }
    if (found.host_) {
        const auto default_port = detail::http_uri_scheme_default_port(request.scheme_);
        if (!http_authorities_equal(request.authority_, found.host_value_, default_port)) {
            return http_connect_udp_error::invalid_request;
        }
    }
    if (request.version_ != http_protocol_version::http11) {
        if (request.method_.view() != "CONNECT" || request.protocol_.view() != "connect-udp" ||
            !is_http_or_https_scheme(request.scheme_) || found.connection_field_ ||
            found.upgrade_field_ || found.http2_forbidden_) {
            return http_connect_udp_error::invalid_request;
        }
    } else if (request.method_.view() != "GET" ||
               (!request.scheme_.empty() && !is_http_or_https_scheme(request.scheme_)) ||
               !found.upgrade_ || !found.connection_ || !found.host_) {
        return http_connect_udp_error::invalid_request;
    }
    return std::nullopt;
}
}  // namespace
std::optional<http_connect_udp_error> validate_http_connect_udp_request(http_connect_udp_request_view request) noexcept {
    return validate_request(request, false);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_request_to_send(
    http_connect_udp_request_view request) noexcept {
    return validate_request(request, true);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header_view> headers) noexcept {
    return validate_connect_udp_response(version, status, headers);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header> headers) noexcept {
    return validate_connect_udp_response(version, status, headers);
}
bool is_http_connect_udp_upgrade_request(const http_request& request) noexcept {
    if (request.protocol_version() != http_protocol_version::http11 ||
        request.known_method() != http_known_method::get) {
        return false;
    }
    detail::http_upgrade_protocols upgrade_protocols;
    bool connect_udp = false;
    const bool valid = detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::upgrade, [&](std::string_view value) noexcept {
            return upgrade_protocols.parse_field(value, detail::http_field_list_role::recipient,
                       [&connect_udp](const detail::http_upgrade_protocol& protocol) noexcept {
                           if (protocol.version_.empty() &&
                               http_ascii_equals_ignore_case(protocol.name_, "connect-udp")) {
                               connect_udp = true;
                           }
                           return true;
                       }) == detail::http_field_list_parse_status::ok;
        });
    return valid && upgrade_protocols.has_protocol() && connect_udp;
}
std::optional<http_connect_udp_error> validate_http_connect_udp_request(const http_request& request) noexcept {
    if (request.target_form() == http_request_target_form::absolute &&
        !detail::is_valid_uri_component(request.query_string(), true, true)) {
        return http_connect_udp_error::invalid_request;
    }
    const auto path = request.target_form() == http_request_target_form::absolute
                          ? request.path()
                          : request.target();
    return validate_http_connect_udp_request({.version_ = request.protocol_version(), .method_ = request.method(), .protocol_ = request.protocol(), .scheme_ = request.scheme(), .authority_ = request.authority().empty() ? detail::http_request_access::known_header(request, detail::http_field_kind::host) : request.authority(), .path_ = path, .headers_ = request.headers()});
}
std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response response, http_protocol_version version) {
    if (version == http_protocol_version::http10 || !response.status().is_successful() ||
        forbids_capsule_protocol(response.status().value()) || response.file_body() || !response.body_bytes().empty()) {
        return http_connect_udp_error::invalid_response;
    }
    // The response's own fields go through the one CONNECT-UDP handshake walk;
    // the protocol, not the application, generates the Upgrade pair.
    const auto parsed_value = fields(std::span(response.headers().begin(), response.headers().size()),
        handshake_message_role::response_sender, http_connect_udp_error::invalid_response);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    if (found.upgrade_field_ || found.connection_field_ || found.forbidden_ ||
        (version != http_protocol_version::http11 && found.http2_forbidden_)) {
        return http_connect_udp_error::invalid_response;
    }
    // As the sender, the planner only lets a single ?1 through.
    if (found.capsule_protocol_ == capsule_protocol_state::invalid) {
        return http_connect_udp_error::invalid_capsule_protocol;
    }
    if (found.capsule_protocol_ == capsule_protocol_state::absent) {
        response.header_stable_view("Capsule-Protocol", "?1");
    }
    if (version == http_protocol_version::http11) {
        response.status_code_ = http_status::switching_protocols;
        response.header_stable_view(
            detail::http_connection_field_name, detail::http_connection_upgrade_option);
        response.header_stable_view("Upgrade", "connect-udp");
    }
    return response;
}
http1_response_head_plan prepare_http1_connect_udp_response_head(const http_response& response) {
    if (response.status() != http_status::switching_protocols || response.file_body() || !response.body_bytes().empty() ||
        validate_connect_udp_response(http_protocol_version::http11, response.status().value(),
            std::span(response.headers().begin(), response.headers().size()))) {
        throw http1_response_head_error("invalid HTTP/1 CONNECT-UDP response head");
    }
    return http1_close_delimited_response_stream_head_plan(plan_http_response_body(http_known_method::get, response.status()), http1_request_connection_plan::http11_close());
}
}  // namespace ruvia

namespace ruvia {
namespace {
// CONNECT-UDP context ID 0 (RFC 9298 section 5) as its one-byte variable-length integer.
constexpr std::string_view udp_payload_context_id{"\0", 1};
}  // namespace
// One DATAGRAM capsule (RFC 9297 section 3.5) whose value is `context_prefix`
// followed by the borrowed payload.
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::plan_datagram_capsule(
    std::span<const char> payload_value, std::string_view context_prefix) noexcept {
    if (payload_value.size() > detail::quic_varint_max - context_prefix.size()) {
        return http_datagram_error::payload_too_large;
    }
    http_datagram_write_plan plan;
    plan.payload_ = payload_value;
    const auto header = encode_http_capsule_header(
        plan.prefix_, http_datagram_capsule_type, context_prefix.size() + payload_value.size());
    if (header.index() != 0) {
        return http_datagram_error::payload_too_large;
    }
    plan.prefix_size_ = std::get<0>(header);
    std::ranges::copy(context_prefix, plan.prefix_.data() + plan.prefix_size_);
    plan.prefix_size_ += context_prefix.size();
    return plan;
}
std::optional<bool> parse_http_capsule_protocol(std::string_view value) noexcept {
    detail::http_structured_parser parser{detail::http_structured_text_input{value}};
    parser.spaces();
    detail::http_structured_item item;
    if (!parser.item(item) || !parser.parameters()) {
        return std::nullopt;
    }
    // RFC 8941 4.2: only SP surrounds a top-level Item; OWS (with HTAB)
    // belongs between List and Dictionary members.
    parser.spaces();
    if (!parser.empty() || item.kind_ != detail::http_structured_item::kind_type::boolean) {
        return std::nullopt;
    }
    return item.boolean_;
}

std::optional<http_udp_datagram_view> decode_http_udp_datagram(std::span<const char> input) noexcept {
    auto id = detail::decode_quic_varint(input);
    if (id.index() != 0) {
        return std::nullopt;
    }
    return http_udp_datagram_view{std::get<0>(id).value_, input.subspan(std::get<0>(id).encoded_bytes_)};
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id) noexcept {
    return detail::encode_quic_varint(output, context_id);
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    if (type > detail::quic_varint_max || length > detail::quic_varint_max) {
        return http_capsule_codec_error::value_out_of_range;
    }
    const auto type_size = detail::quic_varint_size(type);
    const auto length_size = detail::quic_varint_size(length);
    if (output.size() < type_size + length_size) {
        return http_capsule_codec_error::output_too_small;
    }
    (void)detail::encode_quic_varint(output, type);
    (void)detail::encode_quic_varint(output.subspan(type_size), length);
    return type_size + length_size;
}
http_capsule_decoder::http_capsule_decoder(http_capsule_config config)
    : config_(config) {
    if (config.max_capsule_length_ > detail::quic_varint_max) {
        throw std::invalid_argument("capsule length exceeds varint range");
    }
}
http_capsule_status http_capsule_decoder::feed(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, http_capsule_retention retention) {
    return feed_impl(input, fin, callback_value, context_value, false, retention).status_;
}
http_capsule_feed_result http_capsule_decoder::feed_one(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, http_capsule_retention retention) {
    return feed_impl(input, fin, callback_value, context_value, true, retention);
}
http_capsule_feed_result http_capsule_decoder::feed_impl(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, bool one, http_capsule_retention retention) {
    if (feeding_) {
        throw std::logic_error("recursive capsule feed");
    }
    if (status_ != http_capsule_status::need_more_data) {
        return {.status_ = status_};
    }
    struct feeding_guard final {
        bool& value_;
        explicit feeding_guard(bool& value) noexcept
            : value_(value) {
            value_ = true;
        }
        ~feeding_guard() {
            value_ = false;
        }
    } feeding_scope(feeding_);
    const auto initial_size = input.size();
    bool completed{};
    const auto limit_reached = [&] {
        status_ = http_capsule_status::limit;
        return http_capsule_feed_result{.status_ = status_, .consumed_bytes_ = initial_size - input.size()};
    };
    try {
        while (!input.empty()) {
            if (!payload_) {
                header_[header_size_++] = input.front();
                input = input.subspan(1);
                if (type_size_ == 0) {
                    auto type = detail::decode_quic_varint(std::span<const char>(header_).first(header_size_));
                    if ((type.index() != 0)) {
                        continue;
                    }
                    type_ = std::get<0>(type).value_;
                    type_size_ = std::get<0>(type).encoded_bytes_;
                }
                auto length = detail::decode_quic_varint(std::span<const char>(header_).subspan(type_size_, header_size_ - type_size_));
                if ((length.index() != 0)) {
                    continue;
                }
                remaining_ = std::get<0>(length).value_;
                header_size_ = 0;
                type_size_ = 0;
                const bool kept_type = retention == http_capsule_retention::all || type_ == http_datagram_capsule_type;
                if (!kept_type) {
                    // RFC 9297 section 3.2: drop and skip over it, at any length.
                    mode_ = payload_mode::skip;
                } else if (retention == http_capsule_retention::udp_payloads && remaining_ != 0) {
                    // RFC 9298 section 4: an unknown Context ID drops the
                    // datagram, so the limit waits for the Context ID.
                    mode_ = payload_mode::context_id;
                    context_id_size_ = 0;
                } else if (remaining_ > config_.max_capsule_length_) {
                    return limit_reached();
                } else {
                    mode_ = payload_mode::deliver;
                }
                payload_ = remaining_ != 0;
                if (!payload_ && kept_type) {
                    if (callback_value) {
                        callback_value(context_value, {type_, {}, true});
                    }
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else if (mode_ == payload_mode::skip) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
            } else if (mode_ == payload_mode::context_id) {
                context_id_[context_id_size_++] = input.front();
                input = input.subspan(1);
                --remaining_;
                const auto context_id = detail::decode_quic_varint(std::span<const char>(context_id_).first(context_id_size_));
                if (context_id.index() != 0 && remaining_ != 0) {
                    continue;
                }
                payload_ = remaining_ != 0;
                if (context_id.index() == 0 && std::get<0>(context_id).value_ != 0) {
                    mode_ = payload_mode::skip;
                    continue;
                }
                // Context ID 0, or a value too short to hold its Context ID.
                if (context_id_size_ + remaining_ > config_.max_capsule_length_) {
                    return limit_reached();
                }
                mode_ = payload_mode::deliver;
                if (callback_value) {
                    callback_value(context_value, {type_, std::span<const char>(context_id_).first(context_id_size_), !payload_});
                }
                if (!payload_) {
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else {
                auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                auto part = input.first(count);
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
                if (callback_value) {
                    callback_value(context_value, {type_, part, !payload_});
                }
                if (!payload_) {
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            }
        }
        if (fin && input.empty()) {
            status_ = payload_ || header_size_ ? http_capsule_status::truncated : http_capsule_status::end;
        }
        return {.status_ = status_, .consumed_bytes_ = initial_size - input.size(), .capsule_complete_ = completed};
    } catch (...) {
        status_ = http_capsule_status::truncated;
        throw;
    }
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_datagram(
    std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    return plan_datagram_capsule(payload_value, {});
}
std::optional<std::span<const char>> http_datagram_session::receive_datagram(
    std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    return input;
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_udp_datagram(
    std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    if (payload_value.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return plan_datagram_capsule(payload_value, udp_payload_context_id);
}
std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> http_datagram_session::receive_udp_datagram(
    std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    const auto udp = decode_http_udp_datagram(input);
    if (!udp.has_value()) {
        return http_datagram_error::malformed;
    }
    if (udp->context_id_ != 0) {
        return std::nullopt;
    }
    if (udp->payload_.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return *udp;
}
}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] constexpr std::uint32_t rotl32(std::uint32_t value, unsigned int shift) noexcept {
    return (value << shift) | (value >> (32U - shift));
}

[[nodiscard]] constexpr std::uint32_t rotr32(std::uint32_t value, unsigned int shift) noexcept {
    return (value >> shift) | (value << (32U - shift));
}

[[nodiscard]] std::array<char, 32> md5_hex_lower(std::string_view input) noexcept {
    static constexpr std::array<std::uint32_t, 64> k_table = {
        0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU,
        0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
        0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U,
        0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
        0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U,
        0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
        0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
        0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
        0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U,
        0xffeff47dU, 0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
        0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};
    static constexpr std::array<unsigned int, 64> s_table = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    std::uint32_t a0 = 0x67452301U;
    std::uint32_t b0 = 0xefcdab89U;
    std::uint32_t c0 = 0x98badcfeU;
    std::uint32_t d0 = 0x10325476U;

    const auto process_block = [&](const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(block[i * 4]) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24U);
        }
        std::uint32_t a = a0;
        std::uint32_t b = b0;
        std::uint32_t c = c0;
        std::uint32_t d = d0;
        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | (~d));
                g = (7 * i) % 16;
            }
            const std::uint32_t temp = d;
            d = c;
            c = b;
            b = b + rotl32(a + f + k_table[i] + m[g], s_table[i]);
            a = temp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    };

    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t offset = 0;
    while (input.size() - offset >= 64) {
        process_block(bytes + offset);
        offset += 64;
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t rem = input.size() - offset;
    if (rem != 0) {
        std::memcpy(tail.data(), bytes + offset, rem);
    }
    tail[rem] = 0x80U;
    const std::size_t padded_len = rem < 56 ? 64 : 128;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(input.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_len - 8 + i] = static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xffU);
    }
    process_block(tail.data());
    if (padded_len == 128) {
        process_block(tail.data() + 64);
    }

    std::array<std::uint8_t, 16> digest{};
    const std::array<std::uint32_t, 4> words = {a0, b0, c0, d0};
    for (std::size_t i = 0; i < 4; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(words[i] & 0xffU);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 8U) & 0xffU);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 16U) & 0xffU);
        digest[i * 4 + 3] = static_cast<std::uint8_t>((words[i] >> 24U) & 0xffU);
    }

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::array<char, 32> out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out[i * 2] = hex_digits[(digest[i] >> 4U) & 0x0fU];
        out[i * 2 + 1] = hex_digits[digest[i] & 0x0fU];
    }
    return out;
}

[[nodiscard]] std::array<char, 64> sha256_hex_lower(std::string_view input) noexcept {
    static constexpr std::array<std::uint32_t, 64> k_table = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    std::array<std::uint32_t, 8> state = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

    const auto process_block = [&](const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + ch + k_table[i] + w[i];
            const std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    };

    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t offset = 0;
    while (input.size() - offset >= 64) {
        process_block(bytes + offset);
        offset += 64;
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t rem = input.size() - offset;
    if (rem != 0) {
        std::memcpy(tail.data(), bytes + offset, rem);
    }
    tail[rem] = 0x80U;
    const std::size_t padded_len = rem < 56 ? 64 : 128;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(input.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_len - 1 - i] = static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xffU);
    }
    process_block(tail.data());
    if (padded_len == 128) {
        process_block(tail.data() + 64);
    }

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::array<char, 64> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t b = 0; b < 4; ++b) {
            const auto byte =
                static_cast<std::uint8_t>((state[i] >> ((3U - b) * 8U)) & 0xffU);
            const std::size_t pos = (i * 4 + b) * 2;
            out[pos] = hex_digits[(byte >> 4U) & 0x0fU];
            out[pos + 1] = hex_digits[byte & 0x0fU];
        }
    }
    return out;
}

template <typename int_type>
void append_decimal_list(std::string& out, std::span<const int_type> items) {
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            out.push_back('-');
        }
        http_append_unsigned(out, static_cast<std::uint64_t>(items[index]));
    }
}

void append_hex_u16(std::string& out, std::uint16_t value) {
    static constexpr char hex_digits[] = "0123456789abcdef";
    out.push_back(hex_digits[(value >> 12U) & 0x0fU]);
    out.push_back(hex_digits[(value >> 8U) & 0x0fU]);
    out.push_back(hex_digits[(value >> 4U) & 0x0fU]);
    out.push_back(hex_digits[value & 0x0fU]);
}

[[nodiscard]] std::string_view ja4_version_code(std::uint16_t version) noexcept {
    switch (version) {
        case 0x0304U:
            return "13";
        case 0x0303U:
            return "12";
        case 0x0302U:
            return "11";
        case 0x0301U:
            return "10";
        case 0x0300U:
            return "s3";
        case 0x0002U:
            return "s2";
        case 0xfeffU:
            return "d1";
        case 0xfefdU:
            return "d2";
        case 0xfefcU:
            return "d3";
        default:
            return "00";
    }
}

[[nodiscard]] constexpr bool is_ascii_alnum(char ch) noexcept {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

}  // namespace

std::uint16_t tls_client_hello_info::version() const noexcept {
    if (!supported_versions_.empty()) {
        std::uint16_t highest = 0;
        for (const auto ver : supported_versions_) {
            if (ver > highest) {
                highest = ver;
            }
        }
        return highest;
    }
    return legacy_version_;
}

std::string tls_client_hello_info::ja3_raw() const {
    std::string out;
    http_append_unsigned(out, legacy_version_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, cipher_suites_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, extensions_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, supported_groups_);
    out.push_back(',');
    append_decimal_list<std::uint8_t>(out, ec_point_formats_);
    return out;
}

std::string tls_client_hello_info::ja3() const {
    const auto raw = ja3_raw();
    const auto digest = md5_hex_lower(raw);
    return std::string(digest.data(), digest.size());
}

std::string tls_client_hello_info::ja4(bool quic) const {
    std::string out;
    out.reserve(36);

    out.push_back(quic ? 'q' : 't');
    out.append(ja4_version_code(version()));

    const bool has_domain_sni =
        !server_name_.empty() && !ip_address::parse(server_name_).has_value();
    out.push_back(has_domain_sni ? 'd' : 'i');

    const auto append_two_digits = [&out](std::size_t count) {
        const auto clamped = (std::min)(std::size_t{99}, count);
        out.push_back(static_cast<char>('0' + (clamped / 10U)));
        out.push_back(static_cast<char>('0' + (clamped % 10U)));
    };
    append_two_digits(cipher_suites_.size());
    append_two_digits(extensions_.size());

    if (alpn_.empty() || alpn_.front().empty()) {
        out.append("00");
    } else {
        const std::string_view first_alpn = alpn_.front();
        const char first_char = first_alpn.front();
        const char last_char = first_alpn.back();
        if (is_ascii_alnum(first_char) && is_ascii_alnum(last_char)) {
            out.push_back(first_char);
            out.push_back(last_char);
        } else {
            static constexpr char hex_digits[] = "0123456789abcdef";
            const auto first_byte = static_cast<unsigned char>(first_char);
            out.push_back(hex_digits[(first_byte >> 4U) & 0x0fU]);
            out.push_back(hex_digits[first_byte & 0x0fU]);
        }
    }
    out.push_back('_');

    if (cipher_suites_.empty()) {
        out.append("000000000000");
    } else {
        std::vector<std::uint16_t> sorted_ciphers(cipher_suites_);
        std::ranges::sort(sorted_ciphers);
        std::string cipher_list;
        cipher_list.reserve(sorted_ciphers.size() * 5U);
        for (std::size_t index = 0; index < sorted_ciphers.size(); ++index) {
            if (index != 0) {
                cipher_list.push_back(',');
            }
            append_hex_u16(cipher_list, sorted_ciphers[index]);
        }
        const auto hash_b = sha256_hex_lower(cipher_list);
        out.append(hash_b.data(), 12U);
    }
    out.push_back('_');

    std::vector<std::uint16_t> filtered_exts;
    filtered_exts.reserve(extensions_.size());
    for (const auto ext : extensions_) {
        if (ext != 0x0000U && ext != 0x0010U) {
            filtered_exts.push_back(ext);
        }
    }
    std::ranges::sort(filtered_exts);

    if (filtered_exts.empty() && signature_algorithms_.empty()) {
        out.append("000000000000");
    } else {
        std::string ext_sig_list;
        for (std::size_t index = 0; index < filtered_exts.size(); ++index) {
            if (index != 0) {
                ext_sig_list.push_back(',');
            }
            append_hex_u16(ext_sig_list, filtered_exts[index]);
        }
        if (!signature_algorithms_.empty()) {
            ext_sig_list.push_back('_');
            for (std::size_t index = 0; index < signature_algorithms_.size(); ++index) {
                if (index != 0) {
                    ext_sig_list.push_back(',');
                }
                append_hex_u16(ext_sig_list, signature_algorithms_[index]);
            }
        }
        const auto hash_c = sha256_hex_lower(ext_sig_list);
        out.append(hash_c.data(), 12U);
    }

    return out;
}

std::string tls_client_hello_info::peetprint() const {
    std::string out;
    if (!supported_versions_.empty()) {
        append_decimal_list<std::uint16_t>(out, supported_versions_);
    } else {
        http_append_unsigned(out, legacy_version_);
    }
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, cipher_suites_);
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, extensions_);
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, supported_groups_);
    out.push_back('|');
    append_decimal_list<std::uint8_t>(out, ec_point_formats_);
    out.push_back('|');
    for (std::size_t index = 0; index < alpn_.size(); ++index) {
        if (index != 0) {
            out.push_back('-');
        }
        out.append(alpn_[index]);
    }
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, signature_algorithms_);
    return out;
}

std::string tls_client_hello_info::peetprint_hash() const {
    const auto raw = peetprint();
    const auto digest = md5_hex_lower(raw);
    return std::string(digest.data(), digest.size());
}

tls_client_hello_fingerprints tls_client_hello_info::fingerprints(bool quic) const {
    tls_client_hello_fingerprints result;
    result.ja3_raw_ = ja3_raw();
    const auto ja3_md5 = md5_hex_lower(result.ja3_raw_);
    result.ja3_.assign(ja3_md5.data(), ja3_md5.size());
    result.ja4_ = ja4(quic);
    result.peetprint_ = peetprint();
    const auto peet_md5 = md5_hex_lower(result.peetprint_);
    result.peetprint_hash_.assign(peet_md5.data(), peet_md5.size());
    return result;
}

std::optional<tls_client_hello_info> parse_tls_client_hello(
    std::span<const std::byte> message) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(message.data());
    std::size_t size = message.size();

    // Unwrap optional 5-byte TLS record header (ContentType::handshake = 0x16).
    if (size >= 9 && data[0] == 0x16U && data[1] == 0x03U) {
        const std::size_t record_len =
            (static_cast<std::size_t>(data[3]) << 8U) | static_cast<std::size_t>(data[4]);
        if (record_len == size - 5U && data[5] == 0x01U) {
            data += 5U;
            size -= 5U;
        }
    }

    // Handshake message header: msg_type == 1 (ClientHello) + 3-byte length.
    if (size < 4 || data[0] != 0x01U) {
        return std::nullopt;
    }
    const std::size_t body_len = (static_cast<std::size_t>(data[1]) << 16U) |
                                 (static_cast<std::size_t>(data[2]) << 8U) |
                                 static_cast<std::size_t>(data[3]);
    if (body_len != size - 4U) {
        return std::nullopt;
    }

    const std::uint8_t* const body = data + 4U;
    std::size_t cursor = 0;
    const auto read_u8 = [&](std::uint8_t& out) noexcept {
        if (cursor + 1U > body_len) {
            return false;
        }
        out = body[cursor++];
        return true;
    };
    const auto read_u16 = [&](std::uint16_t& out) noexcept {
        if (cursor + 2U > body_len) {
            return false;
        }
        out = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(body[cursor]) << 8U) | body[cursor + 1U]);
        cursor += 2U;
        return true;
    };

    tls_client_hello_info info;
    if (!read_u16(info.legacy_version_)) {
        return std::nullopt;
    }
    // 32-byte ClientHello random.
    if (cursor + 32U > body_len) {
        return std::nullopt;
    }
    cursor += 32U;

    // Session ID (0..32 bytes).
    std::uint8_t session_id_len = 0;
    if (!read_u8(session_id_len) || session_id_len > 32U || cursor + session_id_len > body_len) {
        return std::nullopt;
    }
    cursor += session_id_len;

    // Cipher suites (2-byte byte length, non-empty, even).
    std::uint16_t cipher_bytes = 0;
    if (!read_u16(cipher_bytes) || cipher_bytes < 2U || (cipher_bytes % 2U) != 0 ||
        cursor + cipher_bytes > body_len) {
        return std::nullopt;
    }
    const std::size_t cipher_count = cipher_bytes / 2U;
    info.cipher_suites_.reserve(cipher_count);
    for (std::size_t i = 0; i < cipher_count; ++i) {
        std::uint16_t suite = 0;
        (void)read_u16(suite);
        if (!is_tls_grease_value(suite)) {
            info.cipher_suites_.push_back(suite);
        }
    }

    // Compression methods (1-byte length, non-empty).
    std::uint8_t comp_len = 0;
    if (!read_u8(comp_len) || comp_len == 0 || cursor + comp_len > body_len) {
        return std::nullopt;
    }
    cursor += comp_len;

    if (cursor == body_len) {
        return info;
    }

    std::uint16_t extensions_len = 0;
    if (!read_u16(extensions_len) || cursor + extensions_len != body_len) {
        return std::nullopt;
    }

    while (cursor < body_len) {
        std::uint16_t ext_type = 0;
        std::uint16_t ext_len = 0;
        if (!read_u16(ext_type) || !read_u16(ext_len) || cursor + ext_len > body_len) {
            return std::nullopt;
        }
        const std::uint8_t* const ext_data = body + cursor;
        cursor += ext_len;

        if (is_tls_grease_value(ext_type)) {
            continue;
        }
        info.extensions_.push_back(ext_type);

        switch (ext_type) {
            case 0x0000U: {  // server_name
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len =
                    (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U) {
                    return std::nullopt;
                }
                std::size_t pos = 2U;
                while (pos < ext_len) {
                    if (pos + 3U > ext_len) {
                        return std::nullopt;
                    }
                    const std::uint8_t name_type = ext_data[pos];
                    const std::size_t name_len =
                        (static_cast<std::size_t>(ext_data[pos + 1U]) << 8U) | ext_data[pos + 2U];
                    pos += 3U;
                    if (name_len == 0 || pos + name_len > ext_len) {
                        return std::nullopt;
                    }
                    if (name_type == 0 && info.server_name_.empty()) {
                        info.server_name_.assign(
                            reinterpret_cast<const char*>(ext_data + pos), name_len);
                    }
                    pos += name_len;
                }
                break;
            }
            case 0x000aU: {  // supported_groups
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len =
                    (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 2U; pos < ext_len; pos += 2U) {
                    const auto group = static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(group)) {
                        info.supported_groups_.push_back(group);
                    }
                }
                break;
            }
            case 0x000bU: {  // ec_point_formats
                if (ext_len < 1U) {
                    return std::nullopt;
                }
                const std::size_t list_len = ext_data[0];
                if (list_len == 0 || list_len != ext_len - 1U) {
                    return std::nullopt;
                }
                for (std::size_t pos = 1U; pos < ext_len; ++pos) {
                    info.ec_point_formats_.push_back(ext_data[pos]);
                }
                break;
            }
            case 0x000dU: {  // signature_algorithms
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len =
                    (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 2U; pos < ext_len; pos += 2U) {
                    const auto scheme = static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(scheme)) {
                        info.signature_algorithms_.push_back(scheme);
                    }
                }
                break;
            }
            case 0x0010U: {  // application_layer_protocol_negotiation
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len =
                    (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len == 0 || list_len != ext_len - 2U) {
                    return std::nullopt;
                }
                std::size_t pos = 2U;
                while (pos < ext_len) {
                    const std::size_t proto_len = ext_data[pos++];
                    if (proto_len == 0 || pos + proto_len > ext_len) {
                        return std::nullopt;
                    }
                    const bool grease_alpn =
                        proto_len == 2U &&
                        is_tls_grease_value(static_cast<std::uint16_t>(
                            (static_cast<std::uint16_t>(ext_data[pos]) << 8U) |
                            ext_data[pos + 1U]));
                    if (!grease_alpn) {
                        info.alpn_.emplace_back(
                            reinterpret_cast<const char*>(ext_data + pos), proto_len);
                    }
                    pos += proto_len;
                }
                break;
            }
            case 0x002bU: {  // supported_versions
                if (ext_len < 1U) {
                    return std::nullopt;
                }
                const std::size_t list_len = ext_data[0];
                if (list_len < 2U || list_len != ext_len - 1U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 1U; pos < ext_len; pos += 2U) {
                    const auto ver = static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(ver)) {
                        info.supported_versions_.push_back(ver);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    return info;
}

std::optional<tls_client_hello_info> parse_tls_client_hello(std::string_view message) {
    return parse_tls_client_hello(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(message.data()), message.size()));
}

std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(
    std::span<const std::byte> message, bool quic) {
    const auto parsed = parse_tls_client_hello(message);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->fingerprints(quic);
}

std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(
    std::string_view message, bool quic) {
    const auto parsed = parse_tls_client_hello(message);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->fingerprints(quic);
}

}  // namespace ruvia

namespace ruvia::detail {

// RFC 6265bis section 5.6 storage limits. The sender rejects anything the
// receiving parser would truncate or drop, so set_cookie can find and replace
// cookies it emitted earlier by re-parsing them.
inline constexpr std::size_t max_cookie_name_value_bytes = 4096;
inline constexpr std::size_t max_cookie_attribute_value_bytes = 1024;

[[nodiscard]] inline bool is_valid_cookie_attribute(std::string_view value) noexcept {
    // RFC 6265bis path-value is *av-octet: ASCII %x20-3A / %x3C-7E.
    // HTTP field values can carry HTAB and obs-text, but emitting either inside
    // Path produces a Set-Cookie value outside the server grammar.
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7e || c == ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool is_valid_cookie_domain(std::string_view value) noexcept {
    // RFC 6265bis domain-value is an RFC 1034 subdomain as relaxed by RFC 1123:
    // ASCII LDH labels, each at most 63 octets, with a letter or digit at both
    // ends. The root separators consume the remaining two octets of DNS's
    // 255-octet wire limit, so an unqualified textual name is at most 253.
    return value.empty() || is_valid_dns_domain_name(value);
}

[[nodiscard]] inline std::string_view cookie_priority_token(cookie_priority priority) noexcept {
    switch (priority) {
        case cookie_priority::low:
            return "Low";
        case cookie_priority::medium:
            return "Medium";
        case cookie_priority::high:
            return "High";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookie_same_site_token(cookie_same_site same_site) noexcept {
    switch (same_site) {
        case cookie_same_site::strict:
            return "Strict";
        case cookie_same_site::lax:
            return "Lax";
        case cookie_same_site::none:
            return "None";
    }
    return {};
}

[[nodiscard]] inline bool cookie_attribute_emitted(cookie_attribute_policy policy) {
    switch (policy) {
        case cookie_attribute_policy::omit:
            return false;
        case cookie_attribute_policy::emit:
            return true;
    }
    throw std::invalid_argument("invalid cookie attribute policy");
}

// `secure` and `partitioned` are resolved attribute policies; the plan also
// checks Max-Age before converting it to unsigned storage.
inline void validate_cookie(std::string_view name, std::string_view value, const cookie_options& options,
    bool secure, bool partitioned) {
    if (!is_valid_http_header_name(name)) {
        throw std::invalid_argument("invalid cookie name");
    }
    if (!::ruvia::is_valid_cookie_value(value)) {
        throw std::invalid_argument("invalid cookie value");
    }
    const auto prefix_size = options.prefix_ ? ::ruvia::http_cookie_prefix_text(*options.prefix_).size() : 0;
    if (name.size() > max_cookie_name_value_bytes - prefix_size ||
        value.size() > max_cookie_name_value_bytes - prefix_size - name.size()) {
        throw std::invalid_argument("cookie name and value exceed 4096 bytes");
    }
    // Receivers trim OWS around Path, so edge spaces would not round-trip and
    // trailing SP would end the Set-Cookie field value in whitespace.
    const std::string_view path = options.path_;
    if (!is_valid_cookie_attribute(path) || path.size() > max_cookie_attribute_value_bytes ||
        http_trim_ows(path).size() != path.size()) {
        throw std::invalid_argument("invalid cookie attribute");
    }
    if (!is_valid_cookie_domain(options.domain_)) {
        throw std::invalid_argument("invalid cookie domain");
    }
    if (options.expires_.has_value()) {
        using namespace std::chrono;
        constexpr auto minimum = duration_cast<seconds>(sys_days{year{1601} / January / 1}.time_since_epoch());
        const auto expires = floor<seconds>(options.expires_->time_since_epoch());
        if (expires < minimum) {
            throw std::invalid_argument("cookie Expires must not precede year 1601");
        }
    }
    if (options.priority_ && cookie_priority_token(*options.priority_).empty()) {
        throw std::invalid_argument("invalid cookie Priority");
    }
    if (options.same_site_ && cookie_same_site_token(*options.same_site_).empty()) {
        throw std::invalid_argument("invalid cookie SameSite");
    }
    if (options.same_site_ == cookie_same_site::none && !secure) {
        throw std::invalid_argument("SameSite=None cookie requires Secure");
    }
    if (options.prefix_ && ::ruvia::http_cookie_prefix_text(*options.prefix_).empty()) {
        throw std::invalid_argument("invalid cookie prefix");
    }
    if (partitioned && !secure) {
        throw std::invalid_argument("partitioned cookie requires Secure");
    }
    // User agents apply __Host-/__Secure- constraints case-insensitively. Mirror
    // that receive-side rule for literal wire names so every cookie emitted by
    // this sender can actually be stored. A typed prefix is canonical and is the
    // outermost wire prefix, so it takes precedence over bytes in `name`.
    const auto prefix = options.prefix_ ? options.prefix_ : ::ruvia::cookie_name_prefix(name);
    if (prefix && !::ruvia::cookie_prefix_requirements_met(*prefix, secure, options.path_, options.domain_)) {
        throw std::invalid_argument(*prefix == cookie_prefix::host
                                        ? "__Host- cookie requires Secure, Path=/, and no Domain"
                                        : "__Secure- cookie requires Secure");
    }
}

}  // namespace ruvia::detail

namespace ruvia {

set_cookie_plan::set_cookie_plan(
    std::string_view name, std::string_view value, const cookie_options& options)
    : name_(name),
      value_(value),
      path_(options.path_),
      domain_(options.domain_),
      prefix_text_(options.prefix_ ? http_cookie_prefix_text(*options.prefix_) : std::string_view{}),
      priority_text_(options.priority_ ? detail::cookie_priority_token(*options.priority_) : std::string_view{}),
      same_site_text_(options.same_site_ ? detail::cookie_same_site_token(*options.same_site_) : std::string_view{}),
      http_only_(detail::cookie_attribute_emitted(options.http_only_)),
      secure_(detail::cookie_attribute_emitted(options.secure_)),
      partitioned_(detail::cookie_attribute_emitted(options.partitioned_)) {
    // max-age-av starts with a non-zero digit (RFC 6265 section 4.1.1);
    // an immediate expiry is written as the past deletion Expires instead.
    const bool expire_now = options.max_age_.has_value() && options.max_age_->count() == 0;
    if (expire_now || options.expires_.has_value()) {
        const auto expires = expire_now ? cookie_deletion_expires : *options.expires_;
        const auto seconds = std::chrono::floor<std::chrono::seconds>(expires.time_since_epoch()).count();
        if (!std::in_range<std::time_t>(seconds)) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        const auto date = format_http_date(static_cast<std::time_t>(seconds));
        if (!date) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        expires_size_ = date->size();
        std::memcpy(expires_buffer_.data(), date->data(), expires_size_);
    }
    if (options.max_age_.has_value() && options.max_age_->count() != 0) {
        if (options.max_age_->count() < 0) {
            throw std::invalid_argument("cookie Max-Age must not be negative");
        }
        max_age_value_ = static_cast<std::uint64_t>(options.max_age_->count());
    }

    const auto add_size = [this](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - size_) {
            throw std::length_error("Set-Cookie value is too large");
        }
        size_ += amount;
    };

    // Compute the complete wire length before validating the borrowed views.
    // This keeps a hostile oversized view from reaching a grammar scan and
    // prevents the later uninitialized response-header write from receiving a
    // wrapped length.
    add_size(prefix_text_.size());
    add_size(name_.size());
    add_size(1);
    add_size(value_.size());
    if (!path_.empty()) {
        add_size(std::string_view("; Path=").size());
        add_size(path_.size());
    }
    if (!domain_.empty()) {
        add_size(std::string_view("; Domain=").size());
        add_size(domain_.size());
    }
    if (max_age_value_ != 0) {
        add_size(std::string_view("; Max-Age=").size());
        add_size(http_unsigned_decimal_size(max_age_value_));
    }
    if (expires_size_ != 0) {
        add_size(std::string_view("; Expires=").size());
        add_size(expires_size_);
    }
    if (http_only_) {
        add_size(std::string_view("; HttpOnly").size());
    }
    if (secure_) {
        add_size(std::string_view("; Secure").size());
    }
    if (!same_site_text_.empty()) {
        add_size(std::string_view("; SameSite=").size());
        add_size(same_site_text_.size());
    }
    if (!priority_text_.empty()) {
        add_size(std::string_view("; Priority=").size());
        add_size(priority_text_.size());
    }
    if (partitioned_) {
        add_size(std::string_view("; Partitioned").size());
    }

    detail::validate_cookie(name, value, options, secure_, partitioned_);
}

char* write_cookie_request_pair(char* out, std::string_view name, std::string_view value) {
    const std::string_view written(out, cookie_request_pair_size(name, value));
    const bool name_overlaps = detail::http_text_overlaps(name, written);
    const bool value_overlaps = detail::http_text_overlaps(value, written);
    if (name_overlaps || value_overlaps) {
        // Snapshot only bytes that writing the pair could overwrite.
        const auto name_bytes = name_overlaps ? name.size() : 0;
        const auto value_bytes = value_overlaps ? value.size() : 0;
        std::array<char, 256> stack_snapshot;
        if (value_bytes <= stack_snapshot.size() &&
            name_bytes <= stack_snapshot.size() - value_bytes) {
            if (name_overlaps) {
                std::memcpy(stack_snapshot.data(), name.data(), name_bytes);
            }
            if (value_overlaps) {
                std::memcpy(stack_snapshot.data() + name_bytes, value.data(), value_bytes);
            }
            return write_cookie_request_pair(out,
                name_overlaps ? std::string_view(stack_snapshot.data(), name_bytes) : name,
                value_overlaps ? std::string_view(stack_snapshot.data() + name_bytes, value_bytes) : value);
        }
        std::string snapshot;
        snapshot.reserve(name_bytes + value_bytes);
        if (name_overlaps) {
            snapshot.append(name);
        }
        if (value_overlaps) {
            snapshot.append(value);
        }
        return write_cookie_request_pair(out,
            name_overlaps ? std::string_view(snapshot.data(), name_bytes) : name,
            value_overlaps ? std::string_view(snapshot.data() + name_bytes, value_bytes) : value);
    }
    detail::http_raw_wire_sink sink(out);
    sink.append(name);
    sink.append('=');
    sink.append(value);
    return sink.position();
}

void set_cookie_plan::write(char* cursor_value) const {
    (void)write_fields(cursor_value);
}

set_cookie_plan::written_fields set_cookie_plan::write_fields(char* cursor_value) const noexcept {
    written_fields fields;
    detail::http_raw_wire_sink sink(cursor_value);
    const auto* const name_begin = sink.position();
    sink.append(prefix_text_);
    sink.append(name_);
    fields.wire_name_ = {name_begin, static_cast<std::size_t>(sink.position() - name_begin)};
    sink.append('=');
    sink.append(value_);
    if (!path_.empty()) {
        sink.append("; Path=");
        fields.path_ = {sink.position(), path_.size()};
        sink.append(path_);
    }
    if (!domain_.empty()) {
        sink.append("; Domain=");
        fields.domain_ = {sink.position(), domain_.size()};
        sink.append(domain_);
    }
    if (max_age_value_ != 0) {
        // The constructor sized this slice with http_unsigned_decimal_size().
        sink.append("; Max-Age=");
        sink.append_unsigned(max_age_value_);
    }
    if (expires_size_ != 0) {
        sink.append("; Expires=");
        sink.append(std::string_view(expires_buffer_.data(), expires_size_));
    }
    if (http_only_) {
        sink.append("; HttpOnly");
    }
    if (secure_) {
        sink.append("; Secure");
    }
    if (!same_site_text_.empty()) {
        sink.append("; SameSite=");
        sink.append(same_site_text_);
    }
    if (!priority_text_.empty()) {
        sink.append("; Priority=");
        sink.append(priority_text_);
    }
    if (partitioned_) {
        sink.append("; Partitioned");
    }
    return fields;
}

}  // namespace ruvia

namespace ruvia {

bool is_valid_cookie_value(std::string_view value) noexcept {
    // cookie-value = *cookie-octet / ( DQUOTE *cookie-octet DQUOTE ); the quotes
    // are part of the value (RFC 6265 section 4.1.1).
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7f || c == '"' || c == ',' || c == ';' || c == '\\') {
            return false;
        }
    }
    return true;
}

std::optional<cookie_prefix> cookie_name_prefix(std::string_view name) noexcept {
    const auto starts_with_ignore_case = [name](std::string_view prefix) noexcept {
        return name.size() >= prefix.size() && http_ascii_equals_ignore_case(name.substr(0, prefix.size()), prefix);
    };
    if (starts_with_ignore_case("__Host-")) {
        return cookie_prefix::host;
    }
    if (starts_with_ignore_case("__Secure-")) {
        return cookie_prefix::secure;
    }
    return std::nullopt;
}

bool cookie_prefix_requirements_met(
    cookie_prefix prefix, bool secure, std::string_view path, std::string_view domain) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return secure;
        case cookie_prefix::host:
            return secure && path == "/" && domain.empty();
    }
    return false;
}

std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return "__Secure-";
        case cookie_prefix::host:
            return "__Host-";
    }
    return {};
}

namespace {

[[nodiscard]] bool is_cookie_ip_host(std::string_view host) noexcept {
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        return true;
    }
    return ip_address::parse(host).has_value();
}

[[nodiscard]] std::chrono::system_clock::time_point compute_cookie_expiration(
    std::chrono::system_clock::time_point now, std::int64_t max_age_seconds) noexcept {
    using clock_type = std::chrono::system_clock;
    const auto available =
        std::chrono::duration_cast<std::chrono::seconds>(clock_type::time_point::max() - now);
    if (std::chrono::seconds(max_age_seconds) >= available) {
        return clock_type::time_point::max();
    }
    return now +
           std::chrono::duration_cast<clock_type::duration>(std::chrono::seconds(max_age_seconds));
}

[[nodiscard]] bool satisfies_cookie_name_prefix(
    const http_set_cookie_view& cookie, bool secure) noexcept {
    const auto prefix = cookie_name_prefix(cookie.name());
    return !prefix ||
           cookie_prefix_requirements_met(*prefix, secure, cookie.path(), cookie.domain());
}

}  // namespace

http_cookie_jar::http_cookie_jar(const http_origin_view& origin, http_cookie_jar_options options)
    : http_cookie_jar(origin.scheme(), origin.host(), options) {}

http_cookie_jar::http_cookie_jar(
    http_scheme scheme, std::string_view host, http_cookie_jar_options options)
    : host_(host),
      scheme_(scheme),
      max_cookies_(options.max_cookies_),
      max_cookie_bytes_(options.max_cookie_bytes_) {
    if (!host_.empty() && host_.front() == '[' && host_.back() == ']') {
        host_.erase(host_.size() - 1, 1);
        host_.erase(0, 1);
    }
}

bool http_cookie_jar::cookie_domain_matches(
    std::string_view host, std::string_view domain) noexcept {
    if (domain.empty()) {
        return true;
    }
    if (http_ascii_equals_ignore_case(host, domain)) {
        return true;
    }
    if (is_cookie_ip_host(host)) {
        return false;
    }
    return host.size() > domain.size() && host[host.size() - domain.size() - 1] == '.' &&
           http_ascii_equals_ignore_case(host.substr(host.size() - domain.size()), domain);
}

bool http_cookie_jar::cookie_path_matches(
    std::string_view request_path, std::string_view cookie_path) noexcept {
    if (cookie_path.empty() || cookie_path == "/") {
        return !request_path.empty() && request_path.front() == '/';
    }
    if (!request_path.starts_with(cookie_path)) {
        return false;
    }
    return request_path.size() == cookie_path.size() || cookie_path.back() == '/' ||
           request_path[cookie_path.size()] == '/';
}

std::string_view http_cookie_jar::request_path_only(std::string_view target) noexcept {
    return target.substr(0, target.find_first_of("?#"));
}

std::string_view http_cookie_jar::default_cookie_path(std::string_view target) noexcept {
    const auto path = request_path_only(target);
    if (path.empty() || path.front() != '/') {
        return "/";
    }
    const auto slash = path.rfind('/');
    return slash == 0 || slash == std::string_view::npos ? std::string_view("/")
                                                         : path.substr(0, slash);
}

std::size_t http_cookie_jar::entry_storage_bytes(std::string_view name, std::string_view value,
    std::string_view path, std::string_view domain) noexcept {
    std::size_t total = 0;
    for (const auto field : {name, value, path, domain}) {
        if (field.size() > (std::numeric_limits<std::size_t>::max)() - total) {
            return (std::numeric_limits<std::size_t>::max)();
        }
        total += field.size();
    }
    return total;
}

bool http_cookie_jar::has_capacity(
    std::size_t replaced_bytes, std::size_t replacement_bytes, bool adding) const noexcept {
    if (adding && cookies_.size() >= max_cookies_) {
        return false;
    }
    const auto retained_bytes = cookie_bytes_ - replaced_bytes;
    return replacement_bytes <=
           max_cookie_bytes_ - std::min(retained_bytes, max_cookie_bytes_);
}

void http_cookie_jar::reserve_cookie_slot() {
    if (cookies_.size() < cookies_.capacity()) {
        return;
    }
    const auto remaining = max_cookies_ - cookies_.size();
    const auto growth = std::min(remaining, std::max(std::size_t{4}, cookies_.size()));
    cookies_.reserve(cookies_.size() + growth);
}

void http_cookie_jar::recompute_next_expiry() noexcept {
    next_expiry_.reset();
    for (const auto& cookie : cookies_) {
        if (cookie.expires_.has_value() &&
            (!next_expiry_.has_value() || *cookie.expires_ < *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        }
    }
}

void http_cookie_jar::discard_expired(std::chrono::system_clock::time_point now) {
    std::optional<std::chrono::system_clock::time_point> earliest_expiry;
    std::erase_if(cookies_, [this, now, &earliest_expiry](const stored_cookie& cookie) {
        if (!cookie.expires_.has_value()) {
            return false;
        }
        if (*cookie.expires_ > now) {
            if (!earliest_expiry.has_value() || *cookie.expires_ < *earliest_expiry) {
                earliest_expiry = *cookie.expires_;
            }
            return false;
        }
        cookie_bytes_ -=
            entry_storage_bytes(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
        return true;
    });
    next_expiry_ = earliest_expiry;
}

void http_cookie_jar::clear() noexcept {
    cookies_.clear();
    next_expiry_.reset();
    cookie_bytes_ = 0;
}

void http_cookie_jar::add_configured_cookie(std::string_view name, std::string_view value) {
    cookies_.emplace_back(name, value);
    cookie_bytes_ += entry_storage_bytes(name, value, "/", {});
}

std::optional<http_cookie_jar::received_cookie> http_cookie_jar::accept_set_cookie_view(
    std::string_view request_target, std::string_view set_cookie_value,
    std::chrono::system_clock::time_point now) const {
    const auto parsed = parse_set_cookie(set_cookie_value);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    const bool secure = parsed->has(http_set_cookie_attribute::secure);
    const bool partitioned = parsed->has(http_set_cookie_attribute::partitioned);
    if ((secure && scheme_ != http_scheme::https) ||
        !cookie_domain_matches(host_, parsed->domain()) ||
        (parsed->same_site_none() && !secure) ||
        (partitioned && !secure) ||
        !satisfies_cookie_name_prefix(*parsed, secure)) {
        return std::nullopt;
    }

    received_cookie cookie{
        .name_ = parsed->name(),
        .value_ = parsed->value(),
        .path_ = parsed->path().empty() ? default_cookie_path(request_target) : parsed->path(),
        .domain_ = parsed->domain(),
        .expires_ = std::nullopt,
        .secure_ = secure,
        .partitioned_ = partitioned,
    };
    if (const auto max_age_seconds = parsed->max_age_seconds()) {
        cookie.expired_ = *max_age_seconds <= 0;
        if (!cookie.expired_) {
            cookie.expires_ = compute_cookie_expiration(now, *max_age_seconds);
        }
    } else if (const auto expires_at = parsed->expires()) {
        cookie.expired_ = *expires_at <= std::chrono::system_clock::to_time_t(now);
        if (!cookie.expired_) {
            const auto expiration_limit = std::chrono::system_clock::to_time_t(
                compute_cookie_expiration(now, max_cookie_age_seconds));
            cookie.expires_ =
                std::chrono::system_clock::from_time_t(std::min(*expires_at, expiration_limit));
        }
    }
    return cookie;
}

bool http_cookie_jar::store_received_cookie(const received_cookie& cookie) {
    const auto identity_domain = [this](std::string_view domain) {
        return domain.empty() ? std::string_view(host_) : domain;
    };
    const auto cookie_domain = identity_domain(cookie.domain_);
    const auto match = std::ranges::find_if(cookies_, [&](const stored_cookie& stored) {
        return !stored.configured_ && stored.name_ == cookie.name_ &&
               stored.path_ == cookie.path_ && stored.partitioned_ == cookie.partitioned_ &&
               http_ascii_equals_ignore_case(identity_domain(stored.domain_), cookie_domain);
    });
    const auto replaced_bytes =
        match == cookies_.end()
            ? 0
            : entry_storage_bytes(match->name_, match->value_, match->path_, match->domain_);
    if (cookie.expired_) {
        if (match != cookies_.end()) {
            const bool removed_earliest =
                match->expires_.has_value() && next_expiry_ == match->expires_;
            cookie_bytes_ -= replaced_bytes;
            cookies_.erase(match);
            if (removed_earliest) {
                recompute_next_expiry();
            }
        }
        return true;
    }
    const auto replacement_bytes =
        entry_storage_bytes(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
    if (!has_capacity(replaced_bytes, replacement_bytes, match == cookies_.end())) {
        return false;
    }

    if (match == cookies_.end()) {
        stored_cookie replacement(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
        replacement.expires_ = cookie.expires_;
        replacement.secure_ = cookie.secure_;
        replacement.partitioned_ = cookie.partitioned_;
        replacement.configured_ = false;
        const auto insertion = std::ranges::find_if(cookies_,
            [&cookie](const stored_cookie& stored) { return stored.path_.size() < cookie.path_.size(); });
        const auto insertion_index = insertion - cookies_.begin();
        reserve_cookie_slot();
        cookies_.emplace(cookies_.begin() + insertion_index, std::move(replacement));
        if (cookie.expires_.has_value() &&
            (!next_expiry_.has_value() || *cookie.expires_ < *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        }
    } else {
        const auto old_expires = match->expires_;
        std::optional<std::string> replacement_value;
        if (match->value_ != cookie.value_ && cookie.value_.size() > match->value_.capacity()) {
            replacement_value.emplace(cookie.value_);
        }
        std::optional<std::string> replacement_domain;
        if (match->domain_ != cookie.domain_ && cookie.domain_.size() > match->domain_.capacity()) {
            replacement_domain.emplace(cookie.domain_);
        }
        if (replacement_value.has_value()) {
            match->value_.swap(*replacement_value);
        } else if (match->value_ != cookie.value_) {
            match->value_.assign(cookie.value_);
        }
        if (replacement_domain.has_value()) {
            match->domain_.swap(*replacement_domain);
        } else if (match->domain_ != cookie.domain_) {
            match->domain_.assign(cookie.domain_);
        }
        match->expires_ = cookie.expires_;
        match->secure_ = cookie.secure_;
        match->partitioned_ = cookie.partitioned_;
        if (cookie.expires_.has_value() &&
            (!next_expiry_.has_value() || *cookie.expires_ <= *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        } else if (old_expires.has_value() && next_expiry_ == old_expires) {
            recompute_next_expiry();
        }
    }
    cookie_bytes_ = cookie_bytes_ - replaced_bytes + replacement_bytes;
    return true;
}

bool http_cookie_jar::absorb_set_cookie(std::string_view request_target,
    std::string_view set_cookie_value, std::chrono::system_clock::time_point now) {
    if (!cookies_.empty() && next_expiry_.has_value() && now >= *next_expiry_) {
        discard_expired(now);
    }
    const auto accepted = accept_set_cookie_view(request_target, set_cookie_value, now);
    if (!accepted.has_value()) {
        return false;
    }
    return store_received_cookie(*accepted);
}

void http_cookie_jar::retain_response_cookies(std::string_view request_target,
    std::span<const http_header> headers, std::chrono::system_clock::time_point now) {
    bool discarded_expired = cookies_.empty();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "set-cookie")) {
            continue;
        }
        if (!discarded_expired) {
            discarded_expired = true;
            if (next_expiry_.has_value() && now >= *next_expiry_) {
                discard_expired(now);
            }
        }
        if (const auto accepted = accept_set_cookie_view(request_target, header.value(), now)) {
            (void)store_received_cookie(*accepted);
        }
    }
}

void http_cookie_jar::retain_response_cookies(std::string_view request_target,
    std::span<const http_header_view> headers, std::chrono::system_clock::time_point now) {
    bool discarded_expired = cookies_.empty();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "set-cookie")) {
            continue;
        }
        if (!discarded_expired) {
            discarded_expired = true;
            if (next_expiry_.has_value() && now >= *next_expiry_) {
                discard_expired(now);
            }
        }
        if (const auto accepted = accept_set_cookie_view(request_target, header.value(), now)) {
            (void)store_received_cookie(*accepted);
        }
    }
}

std::string http_cookie_jar::cookie_header(std::string_view request_target,
    bool include_received, std::chrono::system_clock::time_point now) {
    std::string output;
    visit_matching_cookies(request_target, include_received, now,
        [&output](std::string_view name, std::string_view value) {
            append_cookie_request_pair(output, name, value);
        });
    return output;
}

}  // namespace ruvia

namespace ruvia {
namespace {

std::pair<std::string_view, std::string_view> split_attribute(std::string_view value) noexcept {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) {
        return {http_trim_ows(value), {}};
    }
    return {http_trim_ows(value.substr(0, equals)), http_trim_ows(value.substr(equals + 1))};
}

std::optional<std::int64_t> parse_max_age_seconds(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }

    std::int64_t seconds = 0;
    const auto [parsed_value, error] =
        std::from_chars(value.data(), value.data() + value.size(), seconds);
    // Even overflow must consume the complete attribute before saturation.
    if (parsed_value != value.data() + value.size()) {
        return std::nullopt;
    }
    if (error == std::errc::result_out_of_range) {
        return value.front() == '-' ? std::numeric_limits<std::int64_t>::min() : max_cookie_age_seconds;
    }
    if (error != std::errc{}) {
        return std::nullopt;
    }
    return seconds > max_cookie_age_seconds ? max_cookie_age_seconds : seconds;
}

bool contains_rejected_received_cookie_control(std::string_view value) noexcept {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x08 || (byte >= 0x0a && byte <= 0x1f) || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

bool is_cookie_date_delimiter(unsigned char byte) noexcept {
    return byte == 0x09 || (byte >= 0x20 && byte <= 0x2f) || (byte >= 0x3b && byte <= 0x40) ||
           (byte >= 0x5b && byte <= 0x60) || (byte >= 0x7b && byte <= 0x7e);
}

// Consume only on success. Any remaining suffix starts with a non-digit,
// as required by RFC 6265 section 5.1.1 and verified erratum 4148.
std::optional<int> consume_cookie_date_digits(
    std::string_view& value, std::size_t minimum_digits, std::size_t maximum_digits) noexcept {
    std::size_t digits = 0;
    int result_value = 0;
    while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9') {
        if (digits == maximum_digits) {
            return std::nullopt;
        }
        result_value = result_value * 10 + (value[digits] - '0');
        ++digits;
    }
    if (digits < minimum_digits) {
        return std::nullopt;
    }
    value.remove_prefix(digits);
    return result_value;
}

struct cookie_date_time final {
    int hour_;
    int minute_;
    int second_;
};

std::optional<cookie_date_time> parse_cookie_date_time(std::string_view value) noexcept {
    const auto hour = consume_cookie_date_digits(value, 1, 2);
    if (!hour || value.empty() || value.front() != ':') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    const auto minute = consume_cookie_date_digits(value, 1, 2);
    if (!minute || value.empty() || value.front() != ':') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    const auto second = consume_cookie_date_digits(value, 1, 2);
    if (!second) {
        return std::nullopt;
    }
    return cookie_date_time{*hour, *minute, *second};
}

std::optional<int> parse_cookie_date_month(std::string_view value) noexcept {
    if (value.size() < 3) {
        return std::nullopt;
    }
    value = value.substr(0, 3);
    for (std::size_t index = 0; index < detail::http_month_names.size(); ++index) {
        if (http_ascii_equals_ignore_case(value, detail::http_month_names[index])) {
            return static_cast<int>(index + 1);
        }
    }
    return std::nullopt;
}

std::optional<std::time_t> parse_cookie_date(std::string_view value) noexcept {
    std::optional<cookie_date_time> time;
    std::optional<int> day;
    std::optional<int> month;
    std::optional<int> year;

    std::size_t cursor_value = 0;
    while (cursor_value < value.size()) {
        while (cursor_value < value.size() &&
               is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        const auto begin = cursor_value;
        while (cursor_value < value.size() &&
               !is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        if (begin == cursor_value) {
            continue;
        }
        auto token = value.substr(begin, cursor_value - begin);
        if (!time) {
            time = parse_cookie_date_time(token);
            if (time) {
                continue;
            }
        }
        if (!day) {
            day = consume_cookie_date_digits(token, 1, 2);
            if (day) {
                continue;
            }
        }
        if (!month) {
            month = parse_cookie_date_month(token);
            if (month) {
                continue;
            }
        }
        if (!year) {
            year = consume_cookie_date_digits(token, 2, 4);
        }
    }

    if (!time || !day || !month || !year) {
        return std::nullopt;
    }
    if (*year >= 70 && *year <= 99) {
        *year += 1900;
    } else if (*year <= 69) {
        *year += 2000;
    }
    if (*year < 1601 || *day < 1 || *day > 31 || time->hour_ > 23 || time->minute_ > 59 ||
        time->second_ > 59) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, *month, *day, time->hour_, time->minute_, time->second_);
}

}  // namespace

std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept {
    const auto first_end = value.find(';');
    const auto cookie_pair = value.substr(0, first_end);
    // RFC 6265 section 5.2 steps 2 and 5: a pair without '=' or with an empty
    // name makes the user agent ignore the whole set-cookie-string.
    const auto equals = cookie_pair.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }
    const auto name = http_trim_ows(cookie_pair.substr(0, equals));
    const auto cookie_value = http_trim_ows(cookie_pair.substr(equals + 1));
    if (name.empty() || name.size() > detail::max_cookie_name_value_bytes ||
        cookie_value.size() > detail::max_cookie_name_value_bytes - name.size() ||
        contains_rejected_received_cookie_control(name) ||
        contains_rejected_received_cookie_control(cookie_value)) {
        return std::nullopt;
    }

    http_set_cookie_view result(name, cookie_value);
    std::size_t offset = first_end == std::string_view::npos ? value.size() : first_end + 1;
    while (offset < value.size()) {
        const auto end = value.find(';', offset);
        const auto [attribute, argument] = split_attribute(value.substr(
            offset, end == std::string_view::npos ? value.size() - offset : end - offset));
        if (contains_rejected_received_cookie_control(attribute) ||
            contains_rejected_received_cookie_control(argument)) {
            return std::nullopt;
        }
        if (argument.size() <= detail::max_cookie_attribute_value_bytes) {
            switch (attribute.size()) {
                case 4U:
                    if (http_ascii_equals_ignore_case(attribute, "Path")) {
                        // RFC 6265 section 5.2.4: a value that is empty or does not start
                        // with '/' selects the default-path; the last Path attribute wins.
                        result.path_ = argument.empty() || argument.front() != '/' ? std::string_view{} : argument;
                    }
                    break;
                case 6U:
                    if (http_ascii_equals_ignore_case(attribute, "Domain")) {
                        // RFC 6265 section 5.2.3: an empty value ignores this cookie-av; a
                        // leading '.' is dropped, so "Domain=." yields a host-only cookie.
                        // Domain-match against the request host is the storage step's job.
                        if (!argument.empty()) {
                            result.domain_ = argument.front() == '.' ? argument.substr(1) : argument;
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "Secure")) {
                        result.set(http_set_cookie_attribute::secure);
                    }
                    break;
                case 7U:
                    if (http_ascii_equals_ignore_case(attribute, "Expires")) {
                        if (const auto expires = parse_cookie_date(argument)) {
                            result.expires_ = expires;
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "Max-Age")) {
                        if (const auto seconds = parse_max_age_seconds(argument)) {
                            result.max_age_seconds_ = seconds;
                        }
                    }
                    break;
                case 8U:
                    if (http_ascii_equals_ignore_case(attribute, "SameSite")) {
                        if (http_ascii_equals_ignore_case(argument, "Strict")) {
                            result.same_site_ = cookie_same_site::strict;
                        } else if (http_ascii_equals_ignore_case(argument, "Lax")) {
                            result.same_site_ = cookie_same_site::lax;
                        } else if (http_ascii_equals_ignore_case(argument, "None")) {
                            result.same_site_ = cookie_same_site::none;
                        } else {
                            result.same_site_.reset();
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "HttpOnly")) {
                        result.set(http_set_cookie_attribute::http_only);
                    }
                    break;
                case 11U:
                    if (http_ascii_equals_ignore_case(attribute, "Partitioned")) {
                        result.set(http_set_cookie_attribute::partitioned);
                    }
                    break;
                default:
                    break;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    return result;
}

}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] bool is_valid_cors_origin_value(std::string_view origin) noexcept {
    return origin == "null" || is_valid_http_serialized_origin(origin);
}

[[nodiscard]] std::string to_lower_copy(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) {
        ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
    }
    return result;
}

[[nodiscard]] std::string join_tokens(std::span<const std::string> items) {
    std::string output;
    if (items.empty()) {
        return output;
    }
    std::size_t total = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            total += 2U;
        }
        total += items[index].size();
    }
    output.reserve(total);
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            output.append(", ");
        }
        output.append(items[index]);
    }
    return output;
}

[[nodiscard]] bool vary_value_contains_token(
    std::string_view vary_value, std::string_view target_token) noexcept {
    bool found = false;
    detail::http_visit_comma_separated_tokens(vary_value, [&](std::string_view token) noexcept {
        if (token == "*" || http_ascii_equals_ignore_case(token, target_token)) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

[[nodiscard]] bool headers_have_vary_token(
    std::span<const http_header> headers, std::string_view token) noexcept {
    for (const auto& header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Vary") &&
            vary_value_contains_token(header.value(), token)) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool is_http_cors_controlled_response_header(std::string_view name) noexcept {
    switch (name.size()) {
        case 22U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Max-Age");
        case 27U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Origin");
        case 28U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Methods") ||
                   http_ascii_equals_ignore_case(name, "Access-Control-Allow-Headers");
        case 29U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Expose-Headers");
        case 32U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Credentials");
        default:
            return false;
    }
}

void merge_http_vary_tokens(
    std::vector<http_header>& headers, std::span<const std::string_view> tokens) {
    std::vector<std::string_view> missing;
    for (const auto token : tokens) {
        const auto trimmed = http_trim_ows(token);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed == "*") {
            if (!headers_have_vary_token(headers, "*")) {
                std::erase_if(headers, [](const http_header& field) noexcept {
                    return http_ascii_equals_ignore_case(field.name(), "Vary");
                });
                headers.emplace_back("Vary", "*");
            }
            return;
        }
        if (!is_valid_http_header_name(trimmed)) {
            throw std::invalid_argument("Vary token must be a valid HTTP field name");
        }
        if (headers_have_vary_token(headers, trimmed)) {
            continue;
        }
        const bool duplicate = std::ranges::any_of(missing, [trimmed](std::string_view current) noexcept {
            return http_ascii_equals_ignore_case(current, trimmed);
        });
        if (!duplicate) {
            missing.push_back(trimmed);
        }
    }
    if (missing.empty()) {
        return;
    }

    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (!http_ascii_equals_ignore_case(headers[index].name(), "Vary")) {
            continue;
        }
        std::string combined;
        for (const auto& field : headers) {
            if (!http_ascii_equals_ignore_case(field.name(), "Vary")) {
                continue;
            }
            const auto trimmed = http_trim_ows(field.value());
            if (trimmed.empty()) {
                continue;
            }
            if (!combined.empty()) {
                combined.append(", ");
            }
            combined.append(trimmed);
        }
        for (const auto token : missing) {
            if (!combined.empty()) {
                combined.append(", ");
            }
            combined.append(token);
        }
        headers[index] = http_header("Vary", combined);
        for (std::size_t rest = headers.size(); rest > index + 1; --rest) {
            if (http_ascii_equals_ignore_case(headers[rest - 1].name(), "Vary")) {
                headers.erase(headers.begin() + static_cast<std::ptrdiff_t>(rest - 1));
            }
        }
        return;
    }

    std::string value;
    for (std::size_t index = 0; index < missing.size(); ++index) {
        if (index != 0) {
            value.append(", ");
        }
        value.append(missing[index]);
    }
    headers.emplace_back("Vary", value);
}

void merge_http_vary_token(std::vector<http_header>& headers, std::string_view token) {
    merge_http_vary_tokens(headers, std::span<const std::string_view>(&token, 1));
}

http_cors_policy::http_cors_policy(const http_cors_policy_config& config)
    : allow_any_origin_(config.allow_any_origin_),
      reflect_request_headers_(config.reflect_request_headers_),
      allow_credentials_(config.allow_credentials_) {
    const auto add_suffix = [this](std::string_view pattern) {
        if (pattern.empty()) {
            throw std::invalid_argument("CORS origin suffix must not be empty");
        }
        suffix_rule rule;
        auto rest = pattern;
        if (const auto scheme_pos = rest.find("://"); scheme_pos != std::string_view::npos) {
            const auto scheme = rest.substr(0, scheme_pos);
            if (!http_ascii_equals_ignore_case(scheme, "http") &&
                !http_ascii_equals_ignore_case(scheme, "https")) {
                throw std::invalid_argument("CORS origin suffix scheme must be http or https");
            }
            rule.scheme_ = to_lower_copy(scheme);
            rest = rest.substr(scheme_pos + 3U);
        }
        if (rest.starts_with("*.")) {
            rule.subdomain_only_ = true;
            rest.remove_prefix(2U);
        } else if (rest.starts_with('.')) {
            rule.subdomain_only_ = true;
            rest.remove_prefix(1U);
        }
        const auto authority = parse_http_authority(rest);
        if (!authority || authority->host().empty() ||
            authority->port_kind() == http_authority_port_kind::empty ||
            authority->host().starts_with('[') ||
            is_valid_http_ipv4_literal(authority->host()) ||
            authority->host().front() == '.' || authority->host().back() == '.') {
            throw std::invalid_argument("CORS origin suffix must name a valid domain");
        }
        rule.host_suffix_ = to_lower_copy(authority->host());
        rule.port_ = authority->port();
        suffix_rules_.push_back(std::move(rule));
    };

    for (const auto& origin_entry : config.allowed_origins_) {
        const std::string_view view(origin_entry);
        if (view == "*") {
            allow_any_origin_ = true;
            continue;
        }
        if (view.starts_with("*.") || view.starts_with('.') ||
            view.find("://*.") != std::string_view::npos ||
            view.find("://.") != std::string_view::npos) {
            add_suffix(view);
            continue;
        }
        if (!is_valid_cors_origin_value(view)) {
            throw std::invalid_argument("CORS allowed origin must be a WHATWG serialized origin");
        }
        exact_origins_.emplace_back(view);
    }

    for (const auto& suffix_entry : config.allowed_origin_suffixes_) {
        add_suffix(suffix_entry);
    }
    if (exact_origins_.empty() && suffix_rules_.empty()) {
        allow_any_origin_ = true;
    }
    for (const auto& method : config.allowed_methods_) {
        const std::string_view view(method);
        if (view == "*") {
            allow_any_method_ = true;
            continue;
        }
        if (!is_valid_http_method_token(view)) {
            throw std::invalid_argument("CORS allowed method must be a valid HTTP method token");
        }
        allowed_methods_.emplace_back(view);
    }
    if (allowed_methods_.empty()) {
        allow_any_method_ = true;
    }
    joined_allowed_methods_ = join_tokens(allowed_methods_);

    for (const auto& header_name : config.allowed_headers_) {
        const std::string_view view(header_name);
        if (view == "*") {
            reflect_request_headers_ = true;
            continue;
        }
        if (!is_valid_http_header_name(view)) {
            throw std::invalid_argument("CORS allowed header must be a valid HTTP field name");
        }
        allowed_headers_.emplace_back(view);
    }
    if (allowed_headers_.empty()) {
        reflect_request_headers_ = true;
    }
    joined_allowed_headers_ = join_tokens(allowed_headers_);
    for (const auto& expose_name : config.expose_headers_) {
        if (!is_valid_http_header_name(expose_name)) {
            throw std::invalid_argument("CORS exposed header must be a valid HTTP field name");
        }
    }
    joined_expose_headers_ = join_tokens(config.expose_headers_);

    if (config.max_age_.has_value()) {
        if (config.max_age_->count() < 0) {
            throw std::invalid_argument("CORS max age must not be negative");
        }
        std::array<char, http_max_unsigned_decimal_size> buffer{};
        const auto formatted =
            http_format_unsigned(buffer, static_cast<std::uint64_t>(config.max_age_->count()));
        max_age_.assign(formatted);
    }
}

bool http_cors_policy::allows_origin(std::string_view origin) const noexcept {
    if (!is_valid_cors_origin_value(origin)) {
        return false;
    }
    if (allow_any_origin_) {
        return true;
    }
    if (origin == "null") {
        return std::ranges::any_of(exact_origins_, [](std::string_view configured) noexcept {
            return configured == "null";
        });
    }

    const auto scheme_sep = origin.find("://");
    if (scheme_sep == std::string_view::npos) {
        return false;
    }
    const auto scheme = origin.substr(0, scheme_sep);
    const auto authority_text = origin.substr(scheme_sep + 3U);

    for (const auto& configured : exact_origins_) {
        if (configured == "null" || configured.size() != origin.size()) {
            continue;
        }
        const auto conf_sep = configured.find("://");
        if (conf_sep != scheme_sep ||
            !http_ascii_equals_ignore_case(scheme, std::string_view(configured).substr(0, conf_sep))) {
            continue;
        }
        if (http_ascii_equals_ignore_case(
                authority_text, std::string_view(configured).substr(conf_sep + 3U))) {
            return true;
        }
    }

    if (suffix_rules_.empty()) {
        return false;
    }
    const auto authority = parse_http_authority(authority_text);
    if (!authority || authority->host().empty() || authority->host().starts_with('[') ||
        is_valid_http_ipv4_literal(authority->host())) {
        return false;
    }
    const auto host = authority->host();
    const auto port = authority->port();

    for (const auto& rule : suffix_rules_) {
        if (!rule.scheme_.empty() && !http_ascii_equals_ignore_case(scheme, rule.scheme_)) {
            continue;
        }
        if (rule.port_.has_value() && port != rule.port_) {
            continue;
        }
        const std::string_view suffix(rule.host_suffix_);
        if (!rule.subdomain_only_ && http_ascii_equals_ignore_case(host, suffix)) {
            return true;
        }
        if (host.size() > suffix.size() && host[host.size() - suffix.size() - 1U] == '.' &&
            http_ascii_equals_ignore_case(host.substr(host.size() - suffix.size()), suffix)) {
            return true;
        }
    }
    return false;
}

bool http_cors_policy::allows_method(std::string_view method) const noexcept {
    if (!is_valid_http_method_token(method)) {
        return false;
    }
    if (allow_any_method_) {
        return true;
    }
    return std::ranges::any_of(allowed_methods_, [method](std::string_view allowed) noexcept {
        return http_ascii_equals_ignore_case(allowed, method);
    });
}

bool http_cors_policy::allows_request_header(std::string_view name) const noexcept {
    if (!is_valid_http_header_name(name)) {
        return false;
    }
    if (reflect_request_headers_) {
        return true;
    }
    return std::ranges::any_of(allowed_headers_, [name](std::string_view allowed) noexcept {
        return http_ascii_equals_ignore_case(allowed, name);
    });
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(
    http_cors_preflight_input input) const {
    const auto method = input.method_.view();
    const auto origin = input.origin_.view();
    const auto requested_method = http_trim_ows(input.access_control_request_method_.view());
    const auto requested_headers = input.access_control_request_headers_.view();

    if (method != "OPTIONS" || origin.empty() || !allows_origin(origin) ||
        requested_method.empty() || !allows_method(requested_method)) {
        return http_cors_preflight_result();
    }

    std::string reflected_headers;
    if (!http_trim_ows(requested_headers).empty()) {
        http_field_name_list names(requested_headers);
        bool saw_name = false;
        while (const auto header_name = names.next()) {
            saw_name = true;
            if (!allows_request_header(*header_name)) {
                return http_cors_preflight_result();
            }
            if (reflect_request_headers_) {
                if (!reflected_headers.empty()) {
                    reflected_headers.append(", ");
                }
                reflected_headers.append(*header_name);
            }
        }
        if (!saw_name || !names.valid()) {
            return http_cors_preflight_result();
        }
    }

    std::vector<http_header> headers;
    headers.reserve(6);
    const std::string_view allow_origin =
        (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    headers.emplace_back("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        headers.emplace_back("Access-Control-Allow-Credentials", "true");
    }
    const std::string_view allow_methods =
        !joined_allowed_methods_.empty() ? std::string_view(joined_allowed_methods_) : requested_method;
    headers.emplace_back("Access-Control-Allow-Methods", allow_methods);
    if (reflect_request_headers_) {
        if (!reflected_headers.empty()) {
            headers.emplace_back("Access-Control-Allow-Headers", reflected_headers);
        }
    } else if (!joined_allowed_headers_.empty()) {
        headers.emplace_back("Access-Control-Allow-Headers", joined_allowed_headers_);
    }
    if (!max_age_.empty()) {
        headers.emplace_back("Access-Control-Max-Age", max_age_);
    }
    headers.emplace_back("Vary",
        reflect_request_headers_
            ? "Origin, Access-Control-Request-Method, Access-Control-Request-Headers"
            : "Origin, Access-Control-Request-Method");

    return http_cors_preflight_result(true, std::move(headers));
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(
    const http_request& request) const {
    const auto origin = request.header("Origin");
    const auto requested_method = request.header("Access-Control-Request-Method");
    if (!origin.has_value() || !requested_method.has_value()) {
        return http_cors_preflight_result();
    }
    std::string combined_headers;
    for (const auto& field : request.headers()) {
        if (!http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
            continue;
        }
        if (http_trim_ows(field.value()).empty()) {
            return http_cors_preflight_result();
        }
        if (!combined_headers.empty()) {
            combined_headers.append(", ");
        }
        combined_headers.append(field.value());
    }
    return evaluate_preflight({
        .method_ = request.method(),
        .origin_ = *origin,
        .access_control_request_method_ = *requested_method,
        .access_control_request_headers_ = combined_headers,
    });
}

bool http_cors_policy::rewrite_response_headers(
    borrowed_text request_origin, std::vector<http_header>& response_headers) const {
    std::erase_if(response_headers, [](const http_header& field) noexcept {
        return is_http_cors_controlled_response_header(field.name());
    });

    const auto origin = request_origin.view();
    const bool granted = !origin.empty() && allows_origin(origin);
    if (!allow_any_origin_ || allow_credentials_) {
        merge_http_vary_token(response_headers, "Origin");
    }
    if (!granted) {
        return false;
    }

    const std::string_view allow_origin =
        (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    response_headers.emplace_back("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        response_headers.emplace_back("Access-Control-Allow-Credentials", "true");
    }
    if (!joined_expose_headers_.empty()) {
        response_headers.emplace_back("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

bool http_cors_policy::rewrite_response_headers(
    borrowed_text request_origin, http_response_headers& response_headers) const {
    response_headers.remove_header("Access-Control-Allow-Origin");
    response_headers.remove_header("Access-Control-Allow-Credentials");
    response_headers.remove_header("Access-Control-Expose-Headers");
    response_headers.remove_header("Access-Control-Max-Age");
    response_headers.remove_header("Access-Control-Allow-Methods");
    response_headers.remove_header("Access-Control-Allow-Headers");

    const auto origin = request_origin.view();
    const bool granted = !origin.empty() && allows_origin(origin);
    if (!allow_any_origin_ || allow_credentials_) {
        response_headers.add_vary_token("Origin");
    }
    if (!granted) {
        return false;
    }

    const std::string_view allow_origin =
        (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    response_headers.header("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        response_headers.header_stable_view("Access-Control-Allow-Credentials", "true");
    }
    if (!joined_expose_headers_.empty()) {
        response_headers.header("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

bool http_cors_policy::rewrite_response_headers(
    borrowed_text request_origin, http_response& response) const {
    auto staged = response.headers().clone(4);
    const bool granted = rewrite_response_headers(request_origin, staged);
    response.commit_headers(std::move(staged));
    return granted;
}

bool http_cors_policy::apply_response_headers(
    const http_request& request, http_response& response) const {
    const bool options = request.known_method() == http_known_method::options;
    if (options) {
        response.add_vary_token("Origin");
        response.add_vary_token("Access-Control-Request-Method");
        if (reflect_request_headers_) {
            response.add_vary_token("Access-Control-Request-Headers");
        }
    } else if (!allow_any_origin_ || allow_credentials_) {
        response.add_vary_token("Origin");
    }

    const auto origin = request.header("Origin");
    if (origin.has_value() && !allows_origin(*origin)) {
        return false;
    }
    const auto requested_method =
        options ? request.header("Access-Control-Request-Method") : std::nullopt;
    if (requested_method.has_value() && !allows_method(http_trim_ows(*requested_method))) {
        return false;
    }
    const bool preflight = options && origin.has_value() && requested_method.has_value();
    const bool reflect_headers =
        preflight && reflect_request_headers_ &&
        !response.header("Access-Control-Allow-Headers").has_value();
    std::string reflected_headers;
    if (options && request.header("Access-Control-Request-Headers").has_value()) {
        std::size_t request_header_names_size = 0;
        for (const auto& field : request.headers()) {
            if (field.name().size() != 30U ||
                !http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
                continue;
            }
            http_field_name_list names(field.value());
            bool saw_name = false;
            while (const auto header_name = names.next()) {
                saw_name = true;
                if (!allows_request_header(*header_name)) {
                    return false;
                }
                const auto separator_size =
                    request_header_names_size == 0 ? std::size_t{0} : std::size_t{2};
                const auto maximum = (std::numeric_limits<std::size_t>::max)();
                if (header_name->size() > maximum - separator_size ||
                    request_header_names_size > maximum - separator_size - header_name->size()) {
                    return false;
                }
                request_header_names_size += separator_size + header_name->size();
                if (reflect_headers) {
                    if (!reflected_headers.empty()) {
                        reflected_headers.append(", ");
                    }
                    reflected_headers.append(*header_name);
                }
            }
            if (!saw_name || !names.valid()) {
                return false;
            }
        }
    }

    if (!response.header("Access-Control-Allow-Origin").has_value()) {
        if (allow_any_origin_ && !allow_credentials_) {
            response.header_stable_view("Access-Control-Allow-Origin", "*");
        } else if (origin.has_value()) {
            response.header("Access-Control-Allow-Origin", *origin);
        } else if (exact_origins_.size() == 1 && suffix_rules_.empty()) {
            response.header("Access-Control-Allow-Origin", exact_origins_.front());
        }
    }
    if (allow_credentials_ && !response.header("Access-Control-Allow-Credentials").has_value()) {
        response.header_stable_view("Access-Control-Allow-Credentials", "true");
    }

    if (preflight) {
        if (!response.header("Access-Control-Allow-Methods").has_value()) {
            if (!joined_allowed_methods_.empty()) {
                response.header("Access-Control-Allow-Methods", joined_allowed_methods_);
            } else if (const auto allow = response.header("Allow");
                allow.has_value() && !allow->empty()) {
                response.header("Access-Control-Allow-Methods", *allow);
            } else {
                response.header("Access-Control-Allow-Methods", http_trim_ows(*requested_method));
            }
        }
        if (!response.header("Access-Control-Allow-Headers").has_value()) {
            if (!reflect_request_headers_ && !joined_allowed_headers_.empty()) {
                response.header("Access-Control-Allow-Headers", joined_allowed_headers_);
            } else if (reflect_request_headers_ && !reflected_headers.empty()) {
                response.header("Access-Control-Allow-Headers", reflected_headers);
            }
        }
        if (!max_age_.empty() && !response.header("Access-Control-Max-Age").has_value()) {
            response.header("Access-Control-Max-Age", max_age_);
        }
        return true;
    }

    if (!joined_expose_headers_.empty() &&
        !response.header("Access-Control-Expose-Headers").has_value()) {
        response.header("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

}  // namespace ruvia

namespace ruvia {
namespace {

// "[2001:db8::1]:443" -> "2001:db8::1"; "192.0.2.1:8080" -> "192.0.2.1".
[[nodiscard]] std::string_view extract_forwarded_node_address(std::string_view node_value) noexcept {
    node_value = http_trim_quoted_field_value(http_trim_ows(node_value));
    if (!node_value.empty() && node_value.front() == '[') {
        const auto close = node_value.find(']');
        if (close == std::string_view::npos) {
            return {};
        }
        const auto rest = node_value.substr(close + 1);
        if (!rest.empty() && rest.front() != ':') {
            return {};
        }
        return node_value.substr(1, close - 1);
    }
    const auto colon = node_value.find(':');
    if (colon != std::string_view::npos &&
        node_value.find(':', colon + 1) == std::string_view::npos) {
        return node_value.substr(0, colon);
    }
    return node_value;
}

[[nodiscard]] std::optional<http_scheme> parse_forwarded_http_scheme(
    std::string_view token) noexcept {
    token = http_trim_quoted_field_value(http_trim_ows(token));
    if (http_ascii_equals_ignore_case(token, "http")) {
        return http_scheme::http;
    }
    if (http_ascii_equals_ignore_case(token, "https")) {
        return http_scheme::https;
    }
    return std::nullopt;
}

struct forwarded_hop final {
    std::optional<ip_address> address_{};
    std::optional<http_scheme> scheme_{};
};

[[nodiscard]] std::optional<ip_address> parse_hop_address(std::string_view node_value) noexcept {
    const auto address_text = extract_forwarded_node_address(node_value);
    if (address_text.empty()) {
        return std::nullopt;
    }
    const auto parsed = ip_address::parse(address_text);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->normalized_ipv4();
}

[[nodiscard]] forwarded_hop parse_rfc7239_element(std::string_view element) noexcept {
    forwarded_hop hop{};
    bool saw_for = false;
    bool saw_proto = false;
    http_visit_semicolon_parameters_quoted_field(
        element, [&](std::string_view name, std::string_view value) noexcept {
            if (http_ascii_equals_ignore_case(name, "for")) {
                if (saw_for) {
                    hop.address_.reset();
                } else {
                    saw_for = true;
                    hop.address_ = parse_hop_address(value);
                }
            } else if (http_ascii_equals_ignore_case(name, "proto")) {
                if (saw_proto) {
                    hop.scheme_.reset();
                } else {
                    saw_proto = true;
                    hop.scheme_ = parse_forwarded_http_scheme(value);
                }
            }
            return true;
        });
    return hop;
}

class forwarding_chain final {
public:
    void append(const http_client_ip_extractor& extractor, forwarded_hop hop) noexcept {
        if (size_ == 0) {
            leftmost_ = hop;
        }
        const bool trusted = hop.address_.has_value() && extractor.is_trusted_proxy(*hop.address_);
        if (trusted) {
            if (awaiting_nearest_trusted_) {
                nearest_trusted_ = hop.address_;
                awaiting_nearest_trusted_ = false;
            }
        } else {
            client_hop_ = hop;
            client_index_ = size_;
            client_identified_ = hop.address_.has_value();
            has_client_hop_ = true;
            nearest_trusted_.reset();
            awaiting_nearest_trusted_ = true;
        }
        ++size_;
    }

    [[nodiscard]] forwarded_hop client() const noexcept {
        if (!has_client_hop_) {
            return leftmost_;
        }
        return {client_identified_ ? client_hop_.address_ : nearest_trusted_, client_hop_.scheme_};
    }

    [[nodiscard]] std::size_t client_index() const noexcept {
        return has_client_hop_ ? client_index_ : 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    forwarded_hop leftmost_{};
    forwarded_hop client_hop_{};
    std::optional<ip_address> nearest_trusted_{};
    std::size_t client_index_{0};
    std::size_t size_{0};
    bool has_client_hop_{false};
    bool client_identified_{false};
    bool awaiting_nearest_trusted_{false};
};

template <typename header_span_type>
[[nodiscard]] std::optional<http_client_ip_result> resolve_from_header(
    const http_client_ip_extractor& extractor, const ip_address& normalized_peer,
    std::string_view header_name, http_forwarded_header_kind kind,
    header_span_type headers) noexcept {
    if (kind == http_forwarded_header_kind::single_ip) {
        std::string_view single_value{};
        std::size_t line_count = 0;
        for (const auto& field : headers) {
            if (http_ascii_equals_ignore_case(field.name(), header_name)) {
                single_value = field.value();
                ++line_count;
            }
        }
        if (line_count != 1) {
            return std::nullopt;
        }
        const auto trimmed = http_trim_ows(single_value);
        if (trimmed.empty() || trimmed.find(',') != std::string_view::npos) {
            return std::nullopt;
        }
        const auto parsed = parse_hop_address(trimmed);
        if (!parsed.has_value() || extractor.is_trusted_proxy(*parsed)) {
            return std::nullopt;
        }
        return http_client_ip_result{
            .address_ = *parsed, .from_forwarded_header_ = true, .scheme_ = std::nullopt};
    }

    forwarding_chain chain;
    for (const auto& field : headers) {
        if (!http_ascii_equals_ignore_case(field.name(), header_name)) {
            continue;
        }
        http_visit_comma_separated_quoted_field_items(
            field.value(), [&](std::string_view item) noexcept {
                if (item.empty()) {
                    return true;
                }
                if (kind == http_forwarded_header_kind::rfc7239_forwarded) {
                    chain.append(extractor, parse_rfc7239_element(item));
                } else {
                    chain.append(extractor, {.address_ = parse_hop_address(item), .scheme_ = std::nullopt});
                }
                return true;
            });
    }
    if (chain.size() == 0) {
        return std::nullopt;
    }
    auto resolved = chain.client();
    if (kind == http_forwarded_header_kind::comma_separated_list &&
        extractor.trust_x_forwarded_proto() &&
        http_ascii_equals_ignore_case(header_name, "X-Forwarded-For")) {
        std::size_t proto_count = 0;
        std::optional<http_scheme> matched_scheme{};
        const auto target_index = chain.client_index();
        for (const auto& field : headers) {
            if (!http_ascii_equals_ignore_case(field.name(), "X-Forwarded-Proto")) {
                continue;
            }
            http_visit_comma_separated_quoted_field_items(
                field.value(), [&](std::string_view item) noexcept {
                    if (item.empty()) {
                        return true;
                    }
                    if (proto_count == target_index) {
                        matched_scheme = parse_forwarded_http_scheme(item);
                    }
                    ++proto_count;
                    return true;
                });
        }
        if (proto_count == chain.size()) {
            resolved.scheme_ = matched_scheme;
        }
    }
    if (!resolved.address_.has_value() && !resolved.scheme_.has_value()) {
        return std::nullopt;
    }
    return http_client_ip_result{
        .address_ = resolved.address_.value_or(normalized_peer),
        .from_forwarded_header_ = true,
        .scheme_ = resolved.scheme_,
    };
}

[[nodiscard]] http_forwarded_header_kind resolve_header_kind(
    std::string_view name, http_forwarded_header_kind configured) noexcept {
    if (configured != http_forwarded_header_kind::auto_detect) {
        return configured;
    }
    if (http_ascii_equals_ignore_case(name, "Forwarded")) {
        return http_forwarded_header_kind::rfc7239_forwarded;
    }
    if (http_ascii_equals_ignore_case(name, "X-Forwarded-For")) {
        return http_forwarded_header_kind::comma_separated_list;
    }
    return http_forwarded_header_kind::single_ip;
}
}  // namespace

http_client_ip_extractor::http_client_ip_extractor(const http_client_ip_config& config)
    : enabled_(config.enabled_),
      trust_x_forwarded_proto_(config.trust_x_forwarded_proto_) {
    trusted_cidrs_.reserve(config.trusted_proxies_.size());
    for (const auto& cidr_text : config.trusted_proxies_) {
        const auto parsed = ip_cidr::parse(cidr_text);
        if (!parsed.has_value()) {
            throw std::invalid_argument("trusted proxy entry must be a valid IP address or CIDR");
        }
        trusted_cidrs_.push_back(*parsed);
    }

    if (config.forwarding_headers_.empty()) {
        compiled_header xff;
        xff.name_.assign("X-Forwarded-For");
        xff.kind_ = http_forwarded_header_kind::comma_separated_list;
        headers_.push_back(std::move(xff));

        compiled_header fwd;
        fwd.name_.assign("Forwarded");
        fwd.kind_ = http_forwarded_header_kind::rfc7239_forwarded;
        headers_.push_back(std::move(fwd));
        return;
    }

    headers_.reserve(config.forwarding_headers_.size());
    for (const auto& spec : config.forwarding_headers_) {
        if (!is_valid_http_header_name(spec.name_)) {
            throw std::invalid_argument("forwarding header name must be a valid HTTP field name");
        }
        compiled_header entry;
        entry.name_.assign(spec.name_);
        entry.kind_ = resolve_header_kind(spec.name_, spec.kind_);
        headers_.push_back(std::move(entry));
    }
}

bool http_client_ip_extractor::is_trusted_proxy(const ip_address& address) const noexcept {
    return std::ranges::any_of(trusted_cidrs_, [&address](const ip_cidr& cidr) noexcept {
        return cidr.contains(address);
    });
}

bool http_client_ip_extractor::is_trusted_proxy(std::string_view address_text) const noexcept {
    const auto parsed = ip_address::parse(address_text);
    return parsed.has_value() && is_trusted_proxy(*parsed);
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, std::span<const http_header> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present = std::ranges::any_of(headers, [&](const http_header& field) noexcept {
            return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
        });
        if (!present) {
            continue;
        }
        if (const auto extracted = resolve_from_header(
                *this, normalized_peer, header_spec.name_, header_spec.kind_, headers)) {
            return *extracted;
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present =
            std::ranges::any_of(headers, [&](const http_header_view& field) noexcept {
                return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
            });
        if (!present) {
            continue;
        }
        if (const auto extracted = resolve_from_header(
                *this, normalized_peer, header_spec.name_, header_spec.kind_, headers)) {
            return *extracted;
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, std::span<const http_header> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

}  // namespace ruvia
