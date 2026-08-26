#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpContentCoding.h"

namespace ruvia {

namespace detail {
struct HttpContentDecodeResultAccess;
}  // namespace detail

enum class HttpContentEncodeError : std::uint8_t { kEncodedSizeExceeded, kEncoderFailure };

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
        return std::get_if<HttpEncodedContent>(&value_);
    }

    [[nodiscard]] const HttpEncodedContent* encoded() const& noexcept {
        return std::get_if<HttpEncodedContent>(&value_);
    }
    HttpEncodedContent* encoded() && = delete;
    const HttpEncodedContent* encoded() const&& = delete;

    [[nodiscard]] const HttpContentEncodeFailure* failure() const& noexcept {
        return std::get_if<HttpContentEncodeFailure>(&value_);
    }
    const HttpContentEncodeFailure* failure() const&& = delete;

private:
    friend HttpContentEncodeResult encodeHttpContent(
        HttpContentCoding, std::string_view, HttpContentEncodeOptions);

    using Value = std::variant<HttpEncodedContent, HttpContentEncodeFailure>;

    [[nodiscard]] static HttpContentEncodeResult makeEncoded(std::pmr::string bytes) noexcept {
        return HttpContentEncodeResult(HttpEncodedContent(std::move(bytes)));
    }

    [[nodiscard]] static HttpContentEncodeResult makeFailure(
        HttpContentEncodeError error) noexcept {
        return HttpContentEncodeResult(HttpContentEncodeFailure(error));
    }

    explicit HttpContentEncodeResult(HttpEncodedContent encoded) noexcept
        : value_(std::move(encoded)) {}

    explicit HttpContentEncodeResult(HttpContentEncodeFailure failure) noexcept
        : value_(failure) {}

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
        return std::get_if<HttpDecodedContent>(&value_);
    }

    [[nodiscard]] const HttpDecodedContent* decoded() const& noexcept {
        return std::get_if<HttpDecodedContent>(&value_);
    }
    HttpDecodedContent* decoded() && = delete;
    const HttpDecodedContent* decoded() const&& = delete;

    [[nodiscard]] const HttpContentDecodeFailure* failure() const& noexcept {
        return std::get_if<HttpContentDecodeFailure>(&value_);
    }
    const HttpContentDecodeFailure* failure() const&& = delete;

private:
    friend struct detail::HttpContentDecodeResultAccess;
    friend HttpContentDecodeResult decodeHttpContent(
        HttpContentCoding, std::string_view, HttpContentDecodeOptions);

    using Value = std::variant<HttpDecodedContent, HttpContentDecodeFailure>;

    [[nodiscard]] static HttpContentDecodeResult makeDecoded(std::pmr::string bytes) noexcept {
        return HttpContentDecodeResult(HttpDecodedContent(std::move(bytes)));
    }

    [[nodiscard]] static HttpContentDecodeResult makeFailure(
        HttpContentDecodeError error) noexcept {
        return HttpContentDecodeResult(HttpContentDecodeFailure(error));
    }

    explicit HttpContentDecodeResult(HttpDecodedContent decoded) noexcept
        : value_(std::move(decoded)) {}

    explicit HttpContentDecodeResult(HttpContentDecodeFailure failure) noexcept
        : value_(failure) {}

    Value value_;
};

// gzip members and zstd frames are consumed through the end of input; Brotli
// trailing bytes are rejected. The exact decoded-size cap applies across the
// complete representation.
[[nodiscard]] HttpContentDecodeResult decodeHttpContent(
    HttpContentCoding coding, std::string_view input, HttpContentDecodeOptions options);

}  // namespace ruvia
