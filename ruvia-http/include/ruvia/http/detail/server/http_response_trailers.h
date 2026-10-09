#pragma once

#include <concepts>
#include <exception>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_known_headers.h"
#include "ruvia/http/detail/util/ascii_case.h"
#include "ruvia/http/detail/util/http_ows.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_response_trailer_section.h"

namespace ruvia::detail {

// Fields that must never appear in a trailer section because they govern message
// framing, routing, authentication, response controls, or content format
// (RFC 9110 §6.5.1, RFC 9113 §8.1).
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

// Name syntax is established by the field API or the wire parser's token scan.
[[nodiscard]] inline bool response_trailer_content_valid(
    std::string_view name, std::string_view value) noexcept {
    return !is_forbidden_response_trailer_name(name) && is_valid_http_field_value(value);
}

// True if (name, value) is an acceptable response trailer field. Shared by the
// HTTP/1.1 chunked-trailer and HTTP/2 trailing-HEADERS sinks so both transports
// enforce the same rules.
[[nodiscard]] inline bool response_trailer_field_valid(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_field_name(name) && response_trailer_content_valid(name, value);
}

// Visit a parsed HTTP/1 chunked response trailer block. The input is the bytes
// between the terminal zero-size chunk and the empty line that ends the trailer
// section; a final CRLF after the last field is also accepted for standalone
// validation. Values are exposed after HTTP optional whitespace has been
// stripped, matching the normalized public response-trailer contract.
template <typename visitor_type>
[[nodiscard]] inline bool visit_http_response_trailer_fields(
    std::string_view trailers, visitor_type&& visitor) {
    if (trailers.size() > max_http_header_bytes) {
        return false;
    }

    std::size_t cursor_value = 0;
    std::size_t field_count = 0;
    while (cursor_value < trailers.size()) {
        if (field_count == max_http_header_fields) {
            return false;
        }
        ++field_count;

        const auto line_end = trailers.find("\r\n", cursor_value);
        const auto line = line_end == std::string_view::npos
                              ? trailers.substr(cursor_value)
                              : trailers.substr(cursor_value, line_end - cursor_value);
        // Finding the separator also proves that the complete name is a token.
        const auto colon = http_token_prefix_size(line);
        if (colon == 0 || colon == line.size() || line[colon] != ':') {
            return false;
        }

        const auto name = line.substr(0, colon);
        const auto value = http_trim_ows(line.substr(colon + 1));
        if (!response_trailer_content_valid(name, value) || !visitor(name, value)) {
            return false;
        }

        if (line_end == std::string_view::npos) {
            return true;
        }
        cursor_value = line_end + 2;
    }
    return true;
}

[[nodiscard]] inline bool http_response_trailer_block_valid(std::string_view trailers) {
    return visit_http_response_trailer_fields(
        trailers, [](std::string_view, std::string_view) noexcept { return true; });
}

class http_response_trailer_section_result;
[[nodiscard]] http_response_trailer_section_result check_http_response_trailer_section(
    std::span<const http_header_view>) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

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

// Borrowed proof that the complete terminal section passed the shared response-
// trailer rules. Protocol encoders accept this value instead of revalidating raw
// fields independently. The source span must outlive its synchronous consumption.
using http_response_trailer_section = ::ruvia::http_response_trailer_section;

class http_response_trailer_section_failure final {
public:
    [[nodiscard]] http_response_trailer_section_error exception() const noexcept {
        return http_response_trailer_section_error();
    }

private:
    friend class http_response_trailer_section_result;
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
template <typename range_type>
concept http_temporary_owning_response_trailer_range =
    !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
    !std::ranges::borrowed_range<range_type> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, http_header_view>;

template <http_temporary_owning_response_trailer_range headers_type>
http_response_trailer_section_result check_http_response_trailer_section(headers_type&&) noexcept = delete;

// Validate the whole section before head, encoder, output, or stream mutation.
[[nodiscard]] inline http_response_trailer_section_result check_http_response_trailer_section(
    std::span<const http_header_view> trailers) noexcept {
    if (trailers.size() > max_http_header_fields) {
        return http_response_trailer_section_result(http_response_trailer_section_failure());
    }
    http_header_section_size section_size;
    for (const auto& trailer : trailers) {
        if (!response_trailer_field_valid(trailer.name(), trailer.value()) ||
            !section_size.add(trailer.name(), trailer.value())) {
            return http_response_trailer_section_result(http_response_trailer_section_failure());
        }
    }
    return http_response_trailer_section_result(http_response_trailer_section(trailers));
}

template <http_temporary_owning_response_trailer_range headers_type>
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
