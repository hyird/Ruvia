#include "ruvia/http/http_cache.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
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
                } else if (http_ascii_equals_ignore_case(name, "private") &&
                           (!has_argument || detail::is_http_token_or_quoted_string(arg))) {
                    value_.set(cache_control_directive::private_value);
                }
                break;
            case 8U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "no-store")) {
                    value_.set(cache_control_directive::no_store);
                } else if (http_ascii_equals_ignore_case(name, "no-cache") &&
                           (!has_argument || detail::is_http_token_or_quoted_string(arg))) {
                    value_.set(cache_control_directive::no_cache);
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

cache_control parse_cache_control(std::span<const http_header> headers) noexcept {
    cache_control_field_parser parser;
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            parser.update(field.value());
        }
    }
    return parser.finish();
}

cache_control parse_cache_control(std::span<const http_header_view> headers) noexcept {
    cache_control_field_parser parser;
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            parser.update(field.value());
        }
    }
    return parser.finish();
}

cache_control parse_cache_control(const http_request& request) noexcept {
    return parse_cache_control(request.headers());
}

cache_control parse_cache_control(const http_response_headers& headers) noexcept {
    cache_control_field_parser parser;
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            parser.update(field.value());
        }
    }
    return parser.finish();
}

cache_control parse_cache_control(const http_response& response) noexcept {
    return parse_cache_control(response.headers());
}

bool http_cache_control_has_no_transform(std::string_view field_value) noexcept {
    return parse_cache_control(field_value).no_transform();
}

bool http_cache_control_has_no_transform(std::span<const http_header> headers) noexcept {
    return parse_cache_control(headers).no_transform();
}

bool http_cache_control_has_no_transform(std::span<const http_header_view> headers) noexcept {
    return parse_cache_control(headers).no_transform();
}

bool http_cache_control_has_no_transform(const http_request& request) noexcept {
    return parse_cache_control(request).no_transform();
}

bool http_cache_control_has_no_transform(const http_response_headers& headers) noexcept {
    return parse_cache_control(headers).no_transform();
}

bool http_cache_control_has_no_transform(const http_response& response) noexcept {
    return parse_cache_control(response).no_transform();
}

namespace {

void append_preserved_cache_control_directives(
    std::string_view field_value, std::pmr::string& output) {
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
    std::pmr::string& output, std::uint64_t client_max_age) {
    if (!output.empty()) {
        output.append(", ");
    }
    output.append("max-age=");
    http_append_unsigned(output, client_max_age);
}

[[nodiscard]] std::pmr::string normalize_vary_field_value(
    std::string_view raw, std::pmr::memory_resource* resource) {
    const auto trimmed = http_trim_ows(raw);
    std::pmr::string normalized(resource);
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

template <typename header_span_type>
[[nodiscard]] std::optional<std::pmr::string> format_vary_cache_key_impl(
    const http_vary_spec& vary, header_span_type request_headers,
    std::pmr::memory_resource* resource) {
    if (vary.is_wildcard()) {
        return std::nullopt;
    }
    auto* const target_resource = detail::http_pmr_resource_or_default(resource);
    std::pmr::string key(target_resource);
    for (const auto& field_name : vary.field_names()) {
        key.append(field_name);
        key.push_back(':');
        bool first_line = true;
        for (const auto& header : request_headers) {
            if (!http_ascii_equals_ignore_case(header.name(), field_name)) {
                continue;
            }
            if (!first_line) {
                key.append(", ");
            }
            first_line = false;
            const auto normalized = normalize_vary_field_value(header.value(), target_resource);
            key.append(normalized);
        }
        key.push_back('\n');
    }
    return key;
}

[[nodiscard]] bool is_hop_by_hop_for_cache_merge(std::string_view name) noexcept {
    switch (name.size()) {
        case 2U:
            return http_ascii_equals_ignore_case(name, "TE");
        case 7U:
            return http_ascii_equals_ignore_case(name, "Trailer") ||
                   http_ascii_equals_ignore_case(name, "Upgrade");
        case 10U:
            return http_ascii_equals_ignore_case(name, "Connection") ||
                   http_ascii_equals_ignore_case(name, "Keep-Alive");
        case 16U:
            return http_ascii_equals_ignore_case(name, "Proxy-Connection");
        case 17U:
            return http_ascii_equals_ignore_case(name, "Transfer-Encoding");
        case 18U:
            return http_ascii_equals_ignore_case(name, "Proxy-Authenticate");
        case 19U:
            return http_ascii_equals_ignore_case(name, "Proxy-Authorization");
        default:
            return false;
    }
}

template <typename header_span_type>
[[nodiscard]] bool is_nominated_by_connection(
    std::string_view field_name, header_span_type headers) noexcept {
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "Connection")) {
            continue;
        }
        bool matched = false;
        detail::http_visit_comma_separated_quoted(header.value(), [&](std::string_view item) noexcept {
            if (http_ascii_equals_ignore_case(http_trim_ows(item), field_name)) {
                matched = true;
                return false;
            }
            return true;
        });
        if (matched) {
            return true;
        }
    }
    return false;
}

template <typename header_span_type>
void merge_304_headers_impl(
    std::pmr::vector<http_header>& cached_headers, header_span_type not_modified_headers) {
    auto* const resource = cached_headers.get_allocator().resource();
    std::pmr::vector<std::string_view> updated_names(resource);
    for (const auto& field : not_modified_headers) {
        const auto name = field.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(field.value()) ||
            is_hop_by_hop_for_cache_merge(name) || is_http_304_forbidden_header(name) ||
            is_nominated_by_connection(name, not_modified_headers)) {
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
    std::erase_if(cached_headers, [&](const http_header& existing) noexcept {
        return std::ranges::any_of(updated_names, [&](std::string_view updated) noexcept {
            return http_ascii_equals_ignore_case(existing.name(), updated);
        });
    });
    for (const auto& field : not_modified_headers) {
        const auto name = field.name();
        const bool should_copy =
            std::ranges::any_of(updated_names, [name](std::string_view updated) noexcept {
                return http_ascii_equals_ignore_case(name, updated);
            });
        if (should_copy) {
            cached_headers.emplace_back(name, field.value(), resource);
        }
    }
}

}  // namespace

std::pmr::string rewrite_http_cache_control_max_age(std::string_view field_value,
    std::uint64_t client_max_age, std::pmr::memory_resource* resource) {
    std::pmr::string output(detail::http_pmr_resource_or_default(resource));
    append_preserved_cache_control_directives(field_value, output);
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

std::pmr::string rewrite_http_cache_control_max_age(std::span<const http_header> headers,
    std::uint64_t client_max_age, std::pmr::memory_resource* resource) {
    std::pmr::string output(detail::http_pmr_resource_or_default(resource));
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

std::pmr::string rewrite_http_cache_control_max_age(std::span<const http_header_view> headers,
    std::uint64_t client_max_age, std::pmr::memory_resource* resource) {
    std::pmr::string output(detail::http_pmr_resource_or_default(resource));
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

void rewrite_http_response_cache_max_age(
    std::pmr::vector<http_header>& response_headers, std::uint64_t client_max_age) {
    auto* const resource = response_headers.get_allocator().resource();
    auto rewritten = rewrite_http_cache_control_max_age(response_headers, client_max_age, resource);
    std::erase_if(response_headers, [](const http_header& field) noexcept {
        return http_ascii_equals_ignore_case(field.name(), "Cache-Control") ||
               http_ascii_equals_ignore_case(field.name(), "Expires");
    });
    response_headers.emplace_back("Cache-Control", rewritten, resource);
}

void rewrite_http_response_cache_max_age(
    http_response_headers& response_headers, std::uint64_t client_max_age) {
    std::pmr::string output(detail::http_pmr_resource_or_default(nullptr));
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
    auto* const resource = field_names_.get_allocator().resource();
    detail::http_visit_comma_separated_quoted(field_value, [&](std::string_view item) {
        const auto token = http_trim_ows(item);
        if (token.empty()) {
            return true;
        }
        if (token == "*") {
            wildcard_ = true;
            field_names_.clear();
            return false;
        }
        if (!is_valid_http_header_name(token)) {
            return true;
        }
        std::pmr::string lower(token, resource);
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

void http_vary_spec::update_headers(std::span<const http_header> headers) {
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Vary")) {
            update(field.value());
        }
    }
}

void http_vary_spec::update_headers(std::span<const http_header_view> headers) {
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Vary")) {
            update(field.value());
        }
    }
}

void http_vary_spec::update_headers(const http_response_headers& headers) {
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Vary")) {
            update(field.value());
        }
    }
}

http_vary_spec parse_http_vary(
    std::string_view field_value, std::pmr::memory_resource* resource) {
    http_vary_spec spec(resource);
    spec.update(field_value);
    return spec;
}

http_vary_spec parse_http_vary(
    std::span<const http_header> headers, std::pmr::memory_resource* resource) {
    http_vary_spec spec(resource);
    spec.update_headers(headers);
    return spec;
}

http_vary_spec parse_http_vary(
    std::span<const http_header_view> headers, std::pmr::memory_resource* resource) {
    http_vary_spec spec(resource);
    spec.update_headers(headers);
    return spec;
}

http_vary_spec parse_http_vary(
    const http_response_headers& headers, std::pmr::memory_resource* resource) {
    http_vary_spec spec(resource);
    spec.update_headers(headers);
    return spec;
}

http_vary_spec parse_http_vary(
    const http_response& response, std::pmr::memory_resource* resource) {
    return parse_http_vary(response.headers(), resource);
}

std::optional<std::pmr::string> format_http_vary_cache_key(const http_vary_spec& vary,
    std::span<const http_header> request_headers, std::pmr::memory_resource* resource) {
    return format_vary_cache_key_impl(vary, request_headers, resource);
}

std::optional<std::pmr::string> format_http_vary_cache_key(const http_vary_spec& vary,
    std::span<const http_header_view> request_headers, std::pmr::memory_resource* resource) {
    return format_vary_cache_key_impl(vary, request_headers, resource);
}

std::optional<std::pmr::string> format_http_vary_cache_key(const http_vary_spec& vary,
    const http_request& request, std::pmr::memory_resource* resource) {
    return format_vary_cache_key_impl(vary, request.headers(), resource);
}

bool http_vary_requests_match(const http_vary_spec& vary,
    std::span<const http_header> first_request, std::span<const http_header> second_request,
    std::pmr::memory_resource* resource) {
    const auto first_key = format_http_vary_cache_key(vary, first_request, resource);
    const auto second_key = format_http_vary_cache_key(vary, second_request, resource);
    return first_key.has_value() && second_key.has_value() && *first_key == *second_key;
}

bool http_vary_requests_match(const http_vary_spec& vary,
    std::span<const http_header_view> first_request,
    std::span<const http_header_view> second_request, std::pmr::memory_resource* resource) {
    const auto first_key = format_http_vary_cache_key(vary, first_request, resource);
    const auto second_key = format_http_vary_cache_key(vary, second_request, resource);
    return first_key.has_value() && second_key.has_value() && *first_key == *second_key;
}

bool http_vary_requests_match(const http_vary_spec& vary, const http_request& first_request,
    const http_request& second_request, std::pmr::memory_resource* resource) {
    return http_vary_requests_match(
        vary, first_request.headers(), second_request.headers(), resource);
}

std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept {
    const auto trimmed = http_trim_ows(field_value);
    if (trimmed.empty() || trimmed.front() == '"') {
        return std::nullopt;
    }
    return parse_delta_seconds(trimmed);
}

std::uint64_t compute_http_cache_age(std::span<const http_header> response_headers,
    std::time_t request_time, std::time_t response_time, std::time_t now) noexcept {
    std::optional<std::time_t> date_value;
    std::optional<std::uint64_t> age_value;
    for (const auto& field : response_headers) {
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

void sanitize_http_304_response_headers(std::pmr::vector<http_header>& headers) {
    std::erase_if(headers, [](const http_header& field) noexcept {
        return is_http_304_forbidden_header(field.name());
    });
}

void sanitize_http_304_response_headers(http_response_headers& headers) {
    std::pmr::vector<std::pmr::string> to_remove(detail::http_pmr_resource_or_default(nullptr));
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

void merge_http_304_response_headers(std::pmr::vector<http_header>& cached_headers,
    std::span<const http_header> not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}

void merge_http_304_response_headers(std::pmr::vector<http_header>& cached_headers,
    std::span<const http_header_view> not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}

void merge_http_304_response_headers(
    http_response_headers& cached_headers, const http_response_headers& not_modified_headers) {
    auto* const resource = detail::http_pmr_resource_or_default(nullptr);
    std::pmr::vector<http_header_view> views(resource);
    views.reserve(not_modified_headers.size());
    for (const auto& field : not_modified_headers) {
        views.emplace_back(field.name(), field.value());
    }
    std::pmr::vector<std::string_view> updated_names(resource);
    for (const auto& field : views) {
        const auto name = field.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(field.value()) ||
            is_hop_by_hop_for_cache_merge(name) || is_http_304_forbidden_header(name) ||
            is_nominated_by_connection(name, std::span<const http_header_view>(views))) {
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
    for (const auto name : updated_names) {
        cached_headers.remove_header(name);
    }
    for (const auto& field : views) {
        const auto name = field.name();
        const bool should_copy =
            std::ranges::any_of(updated_names, [name](std::string_view updated) noexcept {
                return http_ascii_equals_ignore_case(name, updated);
            });
        if (!should_copy) {
            continue;
        }
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
    }
}

void merge_http_304_response_headers(
    http_response& cached_response, const http_response& not_modified_response) {
    auto staged = cached_response.headers().clone(not_modified_response.headers().size());
    merge_http_304_response_headers(staged, not_modified_response.headers());
    cached_response.commit_headers(std::move(staged));
}

}  // namespace ruvia
