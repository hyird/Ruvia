#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpContentCoding.h"

namespace ruvia {

namespace detail {
struct HttpContentDecodeResultAccess;
struct HttpContentEncodeResultAccess;
}  // namespace detail

enum class HttpContentEncodeError : std::uint8_t { kEncodedSizeExceeded,
    kEncoderFailure };

struct HttpContentEncodeOptions final {
    std::size_t maxEncodedBytes{};
    std::pmr::memory_resource* resource{nullptr};
};

class HttpEncodedContent final {
public:
    HttpEncodedContent(const HttpEncodedContent&) = delete;
    HttpEncodedContent& operator=(const HttpEncodedContent&) = delete;
    HttpEncodedContent(HttpEncodedContent&&) noexcept = default;
    HttpEncodedContent& operator=(HttpEncodedContent&&) = delete;

    [[nodiscard]] std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

    [[nodiscard]] std::pmr::string takeBytes() && noexcept {
        return std::move(bytes_);
    }

private:
    friend class HttpContentEncodeResult;
    friend struct detail::HttpContentEncodeResultAccess;

    explicit HttpEncodedContent(std::pmr::string bytes) noexcept
        : bytes_(std::move(bytes)) {}

    std::pmr::string bytes_;
};

class HttpContentEncodeFailure final {
public:
    [[nodiscard]] constexpr HttpContentEncodeError error() const noexcept {
        return error_;
    }

private:
    friend class HttpContentEncodeResult;
    friend struct detail::HttpContentEncodeResultAccess;

    explicit constexpr HttpContentEncodeFailure(HttpContentEncodeError error) noexcept
        : error_(error) {}

    HttpContentEncodeError error_;
};

// Encoding is transactional: success exclusively owns the complete encoded
// representation, while failure owns only its reason.
class HttpContentEncodeResult final {
public:
    HttpContentEncodeResult(const HttpContentEncodeResult&) = delete;
    HttpContentEncodeResult& operator=(const HttpContentEncodeResult&) = delete;
    HttpContentEncodeResult(HttpContentEncodeResult&&) noexcept = default;
    HttpContentEncodeResult& operator=(HttpContentEncodeResult&&) = delete;

    [[nodiscard]] HttpEncodedContent* encoded() & noexcept {
        return value_ ? &*value_ : nullptr;
    }

    [[nodiscard]] const HttpEncodedContent* encoded() const& noexcept {
        return value_ ? &*value_ : nullptr;
    }
    HttpEncodedContent* encoded() && = delete;
    const HttpEncodedContent* encoded() const&& = delete;

    [[nodiscard]] const HttpContentEncodeFailure* failure() const& noexcept {
        return value_ ? nullptr : &value_.error();
    }
    const HttpContentEncodeFailure* failure() const&& = delete;

private:
    friend struct detail::HttpContentEncodeResultAccess;

    using Value = std::expected<HttpEncodedContent, HttpContentEncodeFailure>;

    explicit HttpContentEncodeResult(HttpEncodedContent encoded) noexcept
        : value_(std::move(encoded)) {}

    explicit HttpContentEncodeResult(HttpContentEncodeFailure failure) noexcept
        : value_(std::unexpected(failure)) {}

    Value value_;
};

// Produces one complete content-coded representation within the exact output
// cap. Zero permits only a zero-byte encoding.
[[nodiscard]] HttpContentEncodeResult encodeHttpContent(
    HttpContentCoding coding, std::string_view input, HttpContentEncodeOptions options);

enum class HttpContentDecodeError : std::uint8_t {
    kUnsupportedCoding,
    kInvalidContent,
    kDecodedSizeExceeded,
    kDecoderFailure
};

struct HttpContentDecodeOptions final {
    std::size_t maxDecodedBytes{};
    std::pmr::memory_resource* resource{nullptr};
};

class HttpDecodedContent final {
public:
    HttpDecodedContent(const HttpDecodedContent&) = delete;
    HttpDecodedContent& operator=(const HttpDecodedContent&) = delete;
    HttpDecodedContent(HttpDecodedContent&&) noexcept = default;
    HttpDecodedContent& operator=(HttpDecodedContent&&) = delete;

    [[nodiscard]] std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

    [[nodiscard]] std::pmr::string takeBytes() && noexcept {
        return std::move(bytes_);
    }

private:
    friend class HttpContentDecodeResult;
    friend struct detail::HttpContentDecodeResultAccess;

    explicit HttpDecodedContent(std::pmr::string bytes) noexcept
        : bytes_(std::move(bytes)) {}

    std::pmr::string bytes_;
};

class HttpContentDecodeFailure final {
public:
    [[nodiscard]] constexpr HttpContentDecodeError error() const noexcept {
        return error_;
    }

private:
    friend class HttpContentDecodeResult;
    friend struct detail::HttpContentDecodeResultAccess;

    explicit constexpr HttpContentDecodeFailure(HttpContentDecodeError error) noexcept
        : error_(error) {}

    HttpContentDecodeError error_;
};

// Decoding is transactional: success exclusively owns the complete decoded
// bytes, while malformed input and size failures expose no partial output.
class HttpContentDecodeResult final {
public:
    HttpContentDecodeResult(const HttpContentDecodeResult&) = delete;
    HttpContentDecodeResult& operator=(const HttpContentDecodeResult&) = delete;
    HttpContentDecodeResult(HttpContentDecodeResult&&) noexcept = default;
    HttpContentDecodeResult& operator=(HttpContentDecodeResult&&) = delete;

    [[nodiscard]] HttpDecodedContent* decoded() & noexcept {
        return value_ ? &*value_ : nullptr;
    }

    [[nodiscard]] const HttpDecodedContent* decoded() const& noexcept {
        return value_ ? &*value_ : nullptr;
    }
    HttpDecodedContent* decoded() && = delete;
    const HttpDecodedContent* decoded() const&& = delete;

    [[nodiscard]] const HttpContentDecodeFailure* failure() const& noexcept {
        return value_ ? nullptr : &value_.error();
    }
    const HttpContentDecodeFailure* failure() const&& = delete;

private:
    friend struct detail::HttpContentDecodeResultAccess;

    using Value = std::expected<HttpDecodedContent, HttpContentDecodeFailure>;

    explicit HttpContentDecodeResult(HttpDecodedContent decoded) noexcept
        : value_(std::move(decoded)) {}

    explicit HttpContentDecodeResult(HttpContentDecodeFailure failure) noexcept
        : value_(std::unexpected(failure)) {}

    Value value_;
};

// gzip members and zstd frames are consumed through the end of input; Brotli
// trailing bytes are rejected. The exact decoded-size cap applies across the
// complete representation.
[[nodiscard]] HttpContentDecodeResult decodeHttpContent(
    HttpContentCoding coding, std::string_view input, HttpContentDecodeOptions options);

}  // namespace ruvia
