#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

class http_response_headers;

namespace detail {
struct http_content_coding_field_result_access;
}  // namespace detail

// Content codings supported by the complete-buffer codec facade. gzip is RFC
// 1952, br is RFC 7932, and zstd follows RFC 8878 with RFC 9659's HTTP window
// limit.
enum class http_content_coding : std::uint8_t {
    identity,
    gzip,
    deflate,
    brotli,
    zstd,
};

[[nodiscard]] std::string_view http_content_coding_token(http_content_coding coding) noexcept;

// RFC 9110 section 8.4.1.3 treats x-gzip as gzip. The token is already trimmed.
[[nodiscard]] inline bool http_is_gzip_coding_token(std::string_view token) noexcept {
    return http_ascii_equals_ignore_case(token, "gzip") ||
           http_ascii_equals_ignore_case(token, "x-gzip");
}

// Codings supported for incoming request bodies, as an Accept-Encoding field value.
[[nodiscard]] inline constexpr std::string_view http_supported_request_content_codings() noexcept {
    return "gzip, deflate, br, zstd";
}

class http_unsupported_content_coding final {
public:
    [[nodiscard]] static constexpr http_status_code status() noexcept {
        return http_status::unsupported_media_type;
    }
};

class http_invalid_content_coding_field final {
public:
    [[nodiscard]] static constexpr http_status_code status() noexcept {
        return http_status::bad_request;
    }
};

// A Content-Encoding field value is an ordered coding stack this library can
// decode, a syntactically valid stack containing an unsupported coding, or
// malformed field syntax. Codings are listed in the order they were applied.
class http_content_coding_field_result final {
public:
    http_content_coding_field_result(const http_content_coding_field_result&) = delete;
    http_content_coding_field_result& operator=(const http_content_coding_field_result&) = delete;
    http_content_coding_field_result(http_content_coding_field_result&&) noexcept = default;
    http_content_coding_field_result& operator=(http_content_coding_field_result&&) = delete;

    [[nodiscard]] std::span<const http_content_coding> codings() const& noexcept {
        if (const auto* codings = std::get_if<std::pmr::vector<http_content_coding>>(&value_)) {
            return *codings;
        }
        return {};
    }
    std::span<const http_content_coding> codings() const&& = delete;

    [[nodiscard]] const http_unsupported_content_coding* unsupported() const& noexcept {
        return std::get_if<http_unsupported_content_coding>(&value_);
    }
    const http_unsupported_content_coding* unsupported() const&& = delete;

    [[nodiscard]] const http_invalid_content_coding_field* invalid() const& noexcept {
        return std::get_if<http_invalid_content_coding_field>(&value_);
    }
    const http_invalid_content_coding_field* invalid() const&& = delete;

private:
    friend struct detail::http_content_coding_field_result_access;

    explicit http_content_coding_field_result(std::pmr::vector<http_content_coding> codings) noexcept
        : value_(std::move(codings)) {}

    explicit constexpr http_content_coding_field_result(
        http_unsupported_content_coding unsupported) noexcept
        : value_(unsupported) {}

    explicit constexpr http_content_coding_field_result(http_invalid_content_coding_field invalid) noexcept
        : value_(invalid) {}

    std::variant<std::pmr::vector<http_content_coding>, http_unsupported_content_coding,
        http_invalid_content_coding_field>
        value_;
};

// Parses one logical Content-Encoding field value using recipient list rules.
// Empty list members are ignored; an empty value therefore means identity.
[[nodiscard]] http_content_coding_field_result parse_http_content_coding(std::string_view value,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Folds every Content-Encoding header line into one recipient-side decision.
[[nodiscard]] http_content_coding_field_result parse_http_content_coding_headers(
    std::span<const http_header> headers,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] http_content_coding_field_result parse_http_content_coding_headers(
    const http_response_headers& headers,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

}  // namespace ruvia
