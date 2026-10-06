#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia {

class HttpResponseHeaders;

namespace detail {
struct HttpContentCodingFieldResultAccess;
}  // namespace detail

// Content codings supported by the complete-buffer codec facade. gzip is RFC
// 1952, br is RFC 7932, and zstd follows RFC 8878 with RFC 9659's HTTP window
// limit.
enum class HttpContentCoding : std::uint8_t {
    kIdentity,
    kGzip,
    deflate,
    kBrotli,
    kZstd,
};

[[nodiscard]] std::string_view httpContentCodingToken(HttpContentCoding coding) noexcept;

// RFC 9110 section 8.4.1.3 treats x-gzip as gzip. The token is already trimmed.
[[nodiscard]] inline bool http_is_gzip_coding_token(std::string_view token) noexcept {
    return httpAsciiEqualsIgnoreCase(token, "gzip") ||
           httpAsciiEqualsIgnoreCase(token, "x-gzip");
}

// Codings supported for incoming request bodies, as an Accept-Encoding field value.
[[nodiscard]] inline constexpr std::string_view httpSupportedRequestContentCodings() noexcept {
    return "gzip, deflate, br, zstd";
}

class HttpUnsupportedContentCoding final {
public:
    [[nodiscard]] static constexpr HttpStatusCode status() noexcept {
        return http_status::kUnsupportedMediaType;
    }
};

class HttpInvalidContentCodingField final {
public:
    [[nodiscard]] static constexpr HttpStatusCode status() noexcept {
        return http_status::kBadRequest;
    }
};

// A Content-Encoding field value is an ordered coding stack this library can
// decode, a syntactically valid stack containing an unsupported coding, or
// malformed field syntax. Codings are listed in the order they were applied.
class HttpContentCodingFieldResult final {
public:
    HttpContentCodingFieldResult(const HttpContentCodingFieldResult&) = delete;
    HttpContentCodingFieldResult& operator=(const HttpContentCodingFieldResult&) = delete;
    HttpContentCodingFieldResult(HttpContentCodingFieldResult&&) noexcept = default;
    HttpContentCodingFieldResult& operator=(HttpContentCodingFieldResult&&) = delete;

    [[nodiscard]] std::span<const HttpContentCoding> codings() const& noexcept {
        if (const auto* codings = std::get_if<std::pmr::vector<HttpContentCoding>>(&value_)) {
            return *codings;
        }
        return {};
    }
    std::span<const HttpContentCoding> codings() const&& = delete;

    [[nodiscard]] const HttpUnsupportedContentCoding* unsupported() const& noexcept {
        return std::get_if<HttpUnsupportedContentCoding>(&value_);
    }
    const HttpUnsupportedContentCoding* unsupported() const&& = delete;

    [[nodiscard]] const HttpInvalidContentCodingField* invalid() const& noexcept {
        return std::get_if<HttpInvalidContentCodingField>(&value_);
    }
    const HttpInvalidContentCodingField* invalid() const&& = delete;

private:
    friend struct detail::HttpContentCodingFieldResultAccess;

    explicit HttpContentCodingFieldResult(std::pmr::vector<HttpContentCoding> codings) noexcept
        : value_(std::move(codings)) {}

    explicit constexpr HttpContentCodingFieldResult(
        HttpUnsupportedContentCoding unsupported) noexcept
        : value_(unsupported) {}

    explicit constexpr HttpContentCodingFieldResult(HttpInvalidContentCodingField invalid) noexcept
        : value_(invalid) {}

    std::variant<std::pmr::vector<HttpContentCoding>, HttpUnsupportedContentCoding,
        HttpInvalidContentCodingField>
        value_;
};

// Parses one logical Content-Encoding field value using recipient list rules.
// Empty list members are ignored; an empty value therefore means identity.
[[nodiscard]] HttpContentCodingFieldResult parseHttpContentCoding(std::string_view value,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Folds every Content-Encoding header line into one recipient-side decision.
[[nodiscard]] HttpContentCodingFieldResult parseHttpContentCodingHeaders(
    std::span<const HttpHeader> headers,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] HttpContentCodingFieldResult parseHttpContentCodingHeaders(
    const HttpResponseHeaders& headers,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

}  // namespace ruvia
