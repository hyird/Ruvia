#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

class http_response_headers;
class http_request;

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

// The coding a trimmed token names, case-insensitively; x-gzip is gzip. Null
// for any other token, including an empty one.
[[nodiscard]] std::optional<http_content_coding> http_content_coding_from_token(std::string_view token) noexcept;

// Parsing is bounded to prevent attacker-controlled unbounded decode stages: a
// longer Content-Encoding stack is reported as unsupported.
inline constexpr std::size_t max_content_codings = 8;

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
    constexpr http_unsupported_content_coding() noexcept = default;
    explicit constexpr http_unsupported_content_coding(bool depth_exceeded) noexcept
        : depth_exceeded_(depth_exceeded) {}

    [[nodiscard]] static constexpr http_status_code status() noexcept {
        return http_status::unsupported_media_type;
    }

    [[nodiscard]] constexpr bool depth_exceeded() const noexcept {
        return depth_exceeded_;
    }

private:
    bool depth_exceeded_{false};
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
    [[nodiscard]] std::span<const http_content_coding> codings() const& noexcept {
        if (const auto* codings = std::get_if<content_coding_sequence>(&value_)) {
            return std::span<const http_content_coding>(codings->values_.data(), codings->size_);
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

    struct content_coding_sequence final {
        std::array<http_content_coding, max_content_codings> values_{};
        std::uint8_t size_{0};
    };

    explicit constexpr http_content_coding_field_result(content_coding_sequence codings) noexcept
        : value_(codings) {}

    explicit constexpr http_content_coding_field_result(
        http_unsupported_content_coding unsupported) noexcept
        : value_(unsupported) {}

    explicit constexpr http_content_coding_field_result(http_invalid_content_coding_field invalid) noexcept
        : value_(invalid) {}

    std::variant<content_coding_sequence, http_unsupported_content_coding,
        http_invalid_content_coding_field>
        value_;
};

// Parses one logical Content-Encoding field value using recipient list rules.
// Empty list members are ignored; an empty value therefore means identity.
[[nodiscard]] http_content_coding_field_result parse_http_content_coding(
    std::string_view value) noexcept;

// Folds every Content-Encoding header line into one recipient-side decision.
[[nodiscard]] http_content_coding_field_result parse_http_content_coding_headers(
    std::span<const http_header> headers) noexcept;
[[nodiscard]] http_content_coding_field_result parse_http_content_coding_headers(
    const http_response_headers& headers) noexcept;

// Uses the parser's classified header descriptors without rescanning wire names.
// Admission keeps Content-Encoding opaque. Inspect invalid()/unsupported()
// before using codings(); an empty span in either error state is not identity.
[[nodiscard]] http_content_coding_field_result request_content_coding(
    const http_request& request) noexcept;
}  // namespace ruvia
