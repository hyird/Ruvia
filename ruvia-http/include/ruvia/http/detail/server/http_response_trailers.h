#pragma once

#include <array>
#include <concepts>
#include <exception>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_known_headers.h"
#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_whitespace.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_response_trailer_section.h"

namespace ruvia::detail {

// Fields that must never appear in a trailer section because they govern message
// framing, routing, authentication, response controls, or content format
// (RFC 9110 §6.5.1).
[[nodiscard]] inline bool is_forbidden_response_trailer_name(std::string_view name) noexcept {
    // The response header classifier is the authoritative set of standardized
    // fields Ruvia manages. RFC 9110 explicitly permits only ETag (section
    // 8.8.3) and Accept-Ranges (section 14.3) from that set in trailers; every
    // other known field lacks trailer permission or controls framing,
    // representation handling, caching, routing, cookies, methods, or CORS.
    if (const auto known = classify_response_header_name(name);
        known != 0 && known != response_header_etag && known != response_header_accept_ranges) {
        return true;
    }

    if (is_forbidden_common_trailer_name(name, classify_request_header(name))) {
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

[[nodiscard]] inline bool is_valid_http_response_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_response_trailer_name(name); });
}

// True if (name, value) is an acceptable HTTP/1.1 chunked response trailer.
[[nodiscard]] inline bool response_trailer_field_valid(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && !is_forbidden_response_trailer_name(name) &&
           is_valid_http_header_value(value);
}

class http_response_trailer_section_result;
[[nodiscard]] http_response_trailer_section_result check_http_response_trailer_section(
    std::span<const http_header_view>) noexcept;

class http_response_trailer_section_failure;

class http_response_trailer_section_error final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "invalid HTTP response trailer section";
    }

private:
    friend class http_response_trailer_section_failure;

    http_response_trailer_section_error() noexcept = default;
};

class http_response_trailer_section_failure final {
public:
    [[nodiscard]] http_response_trailer_section_error exception() const noexcept {
        return http_response_trailer_section_error();
    }

private:
    friend http_response_trailer_section_result check_http_response_trailer_section(
        std::span<const http_header_view>) noexcept;

    http_response_trailer_section_failure() noexcept = default;
};

class http_response_trailer_section_result final {
public:
    [[nodiscard]] const http_response_trailer_section* section() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] const http_response_trailer_section* section() const&& = delete;

    [[nodiscard]] const http_response_trailer_section_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] const http_response_trailer_section_failure* failure() const&& = delete;

private:
    friend http_response_trailer_section_result check_http_response_trailer_section(
        std::span<const http_header_view>) noexcept;

    using value_type = std::variant<http_response_trailer_section, http_response_trailer_section_failure>;

    explicit http_response_trailer_section_result(http_response_trailer_section section) noexcept
        : value_(section) {}

    explicit http_response_trailer_section_result(http_response_trailer_section_failure failure) noexcept
        : value_(failure) {}

    value_type value_;
};

// A validated section retains the caller's header array until the synchronous
// protocol submission completes.  Letting a temporary std::array/vector convert
// to span here would return a proof object whose field storage had already died.
template <http_temporary_owning_range<http_header_view> headers_type>
http_response_trailer_section_result check_http_response_trailer_section(headers_type&&) noexcept = delete;

// Validate the whole section before head, encoder, output, or stream mutation.
[[nodiscard]] inline http_response_trailer_section_result check_http_response_trailer_section(
    std::span<const http_header_view> trailers) noexcept {
    if (trailers.size() > max_http_header_fields) {
        return http_response_trailer_section_result(http_response_trailer_section_failure());
    }
    auto section_size = http_trailer_section_budget();
    for (const auto& trailer : trailers) {
        if (!response_trailer_field_valid(trailer.name(), trailer.value()) ||
            !section_size.add_field(trailer.name(), trailer.value())) {
            return http_response_trailer_section_result(http_response_trailer_section_failure());
        }
    }
    return http_response_trailer_section_result(http_response_trailer_section(trailers));
}

template <http_temporary_owning_range<http_header_view> headers_type>
http_response_trailer_section_result validated_response_trailer_section(headers_type&&) = delete;

// Validate a caller's trailers, throwing the typed failure. The caller keeps the
// returned result: the section it exposes borrows from it.
[[nodiscard]] inline http_response_trailer_section_result validated_response_trailer_section(
    std::span<const http_header_view> trailers) {
    auto result_value = check_http_response_trailer_section(trailers);
    if (const auto* failure = result_value.failure()) {
        throw failure->exception();
    }
    return result_value;
}

}  // namespace ruvia::detail
