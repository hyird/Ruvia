#include "http_proxy_forwarding.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "decimal_text.h"
#include "http_header.h"
#include "http_message.h"
#include "http_request.h"
#include "http_uri_syntax.h"

namespace ruvia {

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
    return http_parse_saturating_decimal(http_trim_ows(field_value), max_saturation);
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

bool is_http_hop_by_hop_field_name(std::string_view field_name) noexcept {
    switch (field_name.size()) {
        case 2U:
            return http_ascii_equals_ignore_case(field_name, "TE");
        case 7U:
            return http_ascii_equals_ignore_case(field_name, "Trailer") || http_ascii_equals_ignore_case(field_name, "Upgrade");
        case 10U:
            return http_ascii_equals_ignore_case(field_name, "Connection") || http_ascii_equals_ignore_case(field_name, "Keep-Alive");
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
    return (option.size() == 5U && http_ascii_equals_ignore_case(option, "close")) || is_http_hop_by_hop_field_name(option);
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
        tokens_.push_back(http_ascii_lowercase(token));
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

std::string http_connection_nominated_fields::rebuild_connection_field(bool retain_upgrade, bool include_non_standard) const {
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

http_connection_nominated_fields parse_http_connection_nominated_fields(std::string_view connection_field_value) {
    http_connection_nominated_fields nominated;
    nominated.update(connection_field_value);
    return nominated;
}
bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name, const http_connection_nominated_fields& nominated, bool retain_upgrade) noexcept {
    if (retain_upgrade && nominated.has_upgrade() && http_ascii_equals_ignore_case(field_name, "Upgrade")) {
        return false;
    }
    return is_http_hop_by_hop_field_name(field_name) || nominated.contains(field_name);
}

bool is_http_hop_by_hop_trailer_field(std::string_view field_name, const http_connection_nominated_fields& nominated) noexcept {
    return is_http_hop_by_hop_field_name(field_name) || nominated.contains(field_name);
}
}  // namespace ruvia
