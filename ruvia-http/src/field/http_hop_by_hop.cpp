#include "ruvia/http/http_hop_by_hop.h"

#include <algorithm>

#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"

#include "field/response_field_validation.h"

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
    auto* const resource = tokens_.get_allocator().resource();
    detail::http_visit_comma_separated_quoted(connection_field_value, [&](std::string_view item) {
        const auto token = http_trim_ows(item);
        if (token.empty() || !is_valid_http_header_name(token)) {
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
        std::pmr::string lower(token, resource);
        for (auto& ch : lower) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        tokens_.push_back(std::move(lower));
        return true;
    });
}

void http_connection_nominated_fields::update_headers(std::span<const http_header> headers) {
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Connection")) {
            update(field.value());
        }
    }
}

void http_connection_nominated_fields::update_headers(std::span<const http_header_view> headers) {
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Connection")) {
            update(field.value());
        }
    }
}

void http_connection_nominated_fields::update_headers(const http_request& request) {
    update_headers(request.headers());
}

void http_connection_nominated_fields::update_headers(const http_response_headers& headers) {
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Connection")) {
            update(field.value());
        }
    }
}

void http_connection_nominated_fields::update_headers(const http_response& response) {
    update_headers(response.headers());
}

bool http_connection_nominated_fields::contains(std::string_view field_name) const noexcept {
    return std::ranges::any_of(tokens_, [field_name](std::string_view token) noexcept {
        return http_ascii_equals_ignore_case(token, field_name);
    });
}

std::pmr::vector<std::pmr::string> http_connection_nominated_fields::non_standard_options(
    std::pmr::memory_resource* resource) const {
    auto* const target_resource = detail::http_pmr_resource_or_default(
        resource != nullptr ? resource : tokens_.get_allocator().resource());
    std::pmr::vector<std::pmr::string> result(target_resource);
    for (const auto& token : tokens_) {
        if (!is_http_standard_connection_option(token)) {
            result.emplace_back(token);
        }
    }
    return result;
}

std::pmr::string http_connection_nominated_fields::rebuild_connection_field(
    bool retain_upgrade, bool include_non_standard, std::pmr::memory_resource* resource) const {
    auto* const target_resource = detail::http_pmr_resource_or_default(
        resource != nullptr ? resource : tokens_.get_allocator().resource());
    std::pmr::string output(target_resource);
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
    std::string_view connection_field_value, std::pmr::memory_resource* resource) {
    http_connection_nominated_fields nominated(resource);
    nominated.update(connection_field_value);
    return nominated;
}

http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::span<const http_header> headers, std::pmr::memory_resource* resource) {
    http_connection_nominated_fields nominated(resource);
    nominated.update_headers(headers);
    return nominated;
}

http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::span<const http_header_view> headers, std::pmr::memory_resource* resource) {
    http_connection_nominated_fields nominated(resource);
    nominated.update_headers(headers);
    return nominated;
}

http_connection_nominated_fields parse_http_connection_nominated_fields(
    const http_request& request, std::pmr::memory_resource* resource) {
    http_connection_nominated_fields nominated(resource);
    nominated.update_headers(request);
    return nominated;
}

http_connection_nominated_fields parse_http_connection_nominated_fields(
    const http_response_headers& headers, std::pmr::memory_resource* resource) {
    http_connection_nominated_fields nominated(resource);
    nominated.update_headers(headers);
    return nominated;
}

http_connection_nominated_fields parse_http_connection_nominated_fields(
    const http_response& response, std::pmr::memory_resource* resource) {
    http_connection_nominated_fields nominated(resource);
    nominated.update_headers(response);
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

std::pmr::vector<std::pmr::string> extract_http_non_standard_connection_options(
    std::string_view connection_field_value, std::pmr::memory_resource* resource) {
    const auto nominated =
        parse_http_connection_nominated_fields(connection_field_value, resource);
    return nominated.non_standard_options(resource);
}

std::pmr::string rebuild_http_connection_field(std::string_view connection_field_value,
    bool retain_upgrade, bool include_non_standard, std::pmr::memory_resource* resource) {
    const auto nominated =
        parse_http_connection_nominated_fields(connection_field_value, resource);
    return nominated.rebuild_connection_field(retain_upgrade, include_non_standard, resource);
}

}  // namespace ruvia
