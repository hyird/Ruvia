#pragma once

#include <cstdint>
#include <string_view>
#include <variant>

#include "ruvia/http/HttpStatus.h"

namespace ruvia {

namespace detail {
struct HttpContentCodingFieldResultAccess;
}  // namespace detail

// Content codings supported by the complete-buffer codec facade. gzip is RFC
// 1952, br is RFC 7932, and zstd follows RFC 8878 with RFC 9659's HTTP window
// limit.
enum class HttpContentCoding : std::uint8_t {
    kIdentity,
    kGzip,
    kBrotli,
    kZstd,
};

[[nodiscard]] std::string_view httpContentCodingToken(HttpContentCoding coding) noexcept;

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

// A Content-Encoding field value is either one coding this library can decode
// (including identity), a syntactically valid but unsupported coding stack, or
// malformed field syntax.
class HttpContentCodingFieldResult final {
public:
    [[nodiscard]] const HttpContentCoding* coding() const& noexcept {
        return std::get_if<HttpContentCoding>(&value_);
    }
    const HttpContentCoding* coding() const&& = delete;

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

    explicit constexpr HttpContentCodingFieldResult(HttpContentCoding coding) noexcept
        : value_(coding) {}

    explicit constexpr HttpContentCodingFieldResult(HttpUnsupportedContentCoding unsupported) noexcept
        : value_(unsupported) {}

    explicit constexpr HttpContentCodingFieldResult(HttpInvalidContentCodingField invalid) noexcept
        : value_(invalid) {}

    std::variant<HttpContentCoding, HttpUnsupportedContentCoding, HttpInvalidContentCodingField> value_;
};

// Parses one logical Content-Encoding field value using recipient list rules.
// Empty list members are ignored; an empty value therefore means identity.
[[nodiscard]] HttpContentCodingFieldResult parseHttpContentCoding(std::string_view value) noexcept;

}  // namespace ruvia
