#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "http_header.h"
#include "http_status.h"

namespace ruvia {

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
[[nodiscard]] std::optional<http_content_coding> parse_http_content_coding_token(std::string_view token) noexcept;

// Parsing is bounded to prevent attacker-controlled unbounded decode stages: a
// longer Content-Encoding stack is reported as unsupported, and a longer
// Transfer-Encoding stack is rejected.
inline constexpr std::size_t max_content_codings = 8;

// RFC 9112 section 7.2: only gzip and deflate (x-gzip folds into gzip) are
// registered compression transfer codings; brotli and zstd are content codings only.
[[nodiscard]] constexpr bool is_http_transfer_coding(http_content_coding coding) noexcept {
    return coding == http_content_coding::gzip || coding == http_content_coding::deflate;
}

// The one coding stack, in application order, for Content-Encoding and
// Transfer-Encoding alike. A bounded inline sequence: it owns no memory
// resource, so a parsed framing plan may outlive the storage its head was
// parsed from.
class http_content_codings final {
public:
    void push_back(http_content_coding coding) {
        if (size_ == max_content_codings) {
            throw std::length_error("too many content codings");
        }
        data_[size_++] = coding;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] const http_content_coding* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return data_.data();
    }
    const http_content_coding* data() const&& = delete;
    [[nodiscard]] const http_content_coding* begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return data_.data();
    }
    const http_content_coding* begin() const&& = delete;
    [[nodiscard]] const http_content_coding* end() const& noexcept RUVIA_LIFETIMEBOUND {
        return data_.data() + size_;
    }
    const http_content_coding* end() const&& = delete;
    [[nodiscard]] const http_content_coding& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return data_[index];
    }
    const http_content_coding& operator[](std::size_t) const&& = delete;

private:
    std::array<http_content_coding, max_content_codings> data_{};
    std::uint8_t size_{0};
};

// Why decoding coded bytes failed, for content and transfer codings alike.
// unsupported_coding is reported only for content codings the build lacks.
enum class http_content_decode_error : std::uint8_t {
    unsupported_coding,
    invalid_content,
    decoded_size_exceeded,
};

// RFC 9110 section 8.4.1.3 treats x-gzip as gzip. The token is already trimmed.
[[nodiscard]] inline bool is_http_gzip_coding_token(std::string_view token) noexcept {
    return http_ascii_equals_ignore_case(token, "gzip") || http_ascii_equals_ignore_case(token, "x-gzip");
}

// Codings supported for incoming request bodies, as an Accept-Encoding field value.
[[nodiscard]] inline constexpr std::string_view http_supported_request_content_codings() noexcept {
    return "gzip, deflate, br, zstd";
}

class http_unsupported_content_coding final {
public:
    constexpr http_unsupported_content_coding() noexcept = default;
    explicit constexpr http_unsupported_content_coding(bool depth_exceeded) noexcept :
        depth_exceeded_(depth_exceeded) {}

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
    http_content_coding_field_result(const http_content_coding_field_result&) = default;
    http_content_coding_field_result(http_content_coding_field_result&&) noexcept = default;
    http_content_coding_field_result& operator=(const http_content_coding_field_result&) = delete;
    http_content_coding_field_result& operator=(http_content_coding_field_result&&) = delete;

    [[nodiscard]] std::span<const http_content_coding> codings() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* codings = std::get_if<http_content_codings>(&value_)) {
            return std::span<const http_content_coding>(codings->data(), codings->size());
        }
        return {};
    }
    std::span<const http_content_coding> codings() const&& = delete;

    [[nodiscard]] const http_unsupported_content_coding* unsupported() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_unsupported_content_coding>(&value_);
    }
    const http_unsupported_content_coding* unsupported() const&& = delete;

    [[nodiscard]] const http_invalid_content_coding_field* invalid() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_invalid_content_coding_field>(&value_);
    }
    const http_invalid_content_coding_field* invalid() const&& = delete;

private:
    friend struct detail::http_content_coding_field_result_access;

    explicit http_content_coding_field_result(const http_content_codings& codings) noexcept :
        value_(codings) {}

    explicit constexpr http_content_coding_field_result(http_unsupported_content_coding unsupported) noexcept :
        value_(unsupported) {}

    explicit constexpr http_content_coding_field_result(http_invalid_content_coding_field invalid) noexcept :
        value_(invalid) {}

    std::variant<http_content_codings, http_unsupported_content_coding, http_invalid_content_coding_field> value_;
};

// Parses one logical Content-Encoding field value using recipient list rules.
// Empty list members are ignored; an empty value therefore means identity.
[[nodiscard]] http_content_coding_field_result parse_http_content_coding(std::string_view value) noexcept;

// Folds every Content-Encoding header line into one recipient-side decision.
// Empty list members are ignored; no field or only empty members means identity.
[[nodiscard]] http_content_coding_field_result parse_http_content_coding_headers(const http_header_range_view& headers) noexcept;

// Uses the parser's classified header descriptors without rescanning wire names.
// Admission keeps Content-Encoding opaque. Inspect invalid()/unsupported()
// before using codings(); an empty span in either error state is not identity.
[[nodiscard]] http_content_coding_field_result parse_http_request_content_coding(const http_request& request) noexcept;

// Allowlist compressibility policy for dynamic response compression and static
// precompression: text/*, +json, +xml, and common text/font/wasm/icon media
// types, excluding any type naming a zip container.
[[nodiscard]] bool is_http_media_type_compressible(std::string_view content_type_value) noexcept;

}  // namespace ruvia
