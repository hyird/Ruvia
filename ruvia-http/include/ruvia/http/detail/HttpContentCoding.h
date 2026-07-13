#pragma once

#include "ruvia/http/detail/HeaderAcceptUtils.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ruvia {
class HttpClientResponse;
}

namespace ruvia::detail {

[[nodiscard]] std::string_view httpContentCodingToken(HttpContentCoding coding) noexcept;

class HttpUnsupportedContentCoding final {
public:
    [[nodiscard]] static constexpr std::uint16_t status() noexcept {
        return 415;
    }
};

[[nodiscard]] inline constexpr std::string_view
httpSupportedRequestContentCodings() noexcept {
    return "gzip, br, zstd";
}

// A Content-Encoding field section is either one coding this library can
// decode (including the identity representation), or a coding stack it cannot
// decode completely. The result cannot collapse unsupported wire metadata into
// identity.
class HttpContentCodingFieldResult final {
public:
    explicit HttpContentCodingFieldResult(HttpContentCoding coding) noexcept
        : value_(coding) {}

    explicit HttpContentCodingFieldResult(
        HttpUnsupportedContentCoding unsupported) noexcept
        : value_(unsupported) {}

    [[nodiscard]] const HttpContentCoding* coding() const & noexcept {
        return std::get_if<HttpContentCoding>(&value_);
    }
    const HttpContentCoding* coding() const && = delete;

    [[nodiscard]] const HttpUnsupportedContentCoding* unsupported() const & noexcept {
        return std::get_if<HttpUnsupportedContentCoding>(&value_);
    }
    const HttpUnsupportedContentCoding* unsupported() const && = delete;

private:
    std::variant<HttpContentCoding, HttpUnsupportedContentCoding> value_;
};

// Accumulates the list grammar across every Content-Encoding field line (RFC
// 9110 section 8.4). Empty list members are ignored. Ruvia currently decodes
// exactly one gzip, br, or zstd coding; an unknown token or a stack with more
// than one coding is explicit unsupported metadata.
class HttpContentCodingFieldParser final {
public:
    void update(std::string_view value) noexcept;

    [[nodiscard]] HttpContentCodingFieldResult finish() const noexcept;

private:
    HttpContentCoding coding_{HttpContentCoding::kIdentity};
    std::size_t codingCount_{0};
    bool unsupported_{false};
};

[[nodiscard]] HttpContentCodingFieldResult httpContentCodingFromFieldValue(
    std::string_view value) noexcept;

template <typename Headers>
[[nodiscard]] inline HttpContentCodingFieldResult httpContentCodingFromHeaders(
    const Headers& headers) noexcept {
    HttpContentCodingFieldParser parser;
    for (const auto& header : headers) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Content-Encoding")) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

enum class HttpContentEncodeError : std::uint8_t {
    kEncodedSizeExceeded,
    kEncoderFailure
};

class HttpEncodedContent final {
public:
    HttpEncodedContent(const HttpEncodedContent&) = delete;
    HttpEncodedContent& operator=(const HttpEncodedContent&) = delete;
    HttpEncodedContent(HttpEncodedContent&&) noexcept = default;
    HttpEncodedContent& operator=(HttpEncodedContent&&) = delete;

    [[nodiscard]] std::string_view bytes() const & noexcept {
        return std::string_view(bytes_.data(), bytes_.size());
    }
    std::string_view bytes() const && = delete;

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

    explicit constexpr HttpContentEncodeFailure(
        HttpContentEncodeError error) noexcept
        : error_(error) {}

    HttpContentEncodeError error_;
};

// Encoding is transactional: success exclusively owns the complete encoded
// representation, while failure owns only its reason. The exact output cap is
// part of the operation, so no partial bytes escape when a coding cannot finish
// within it.
class HttpContentEncodeResult final {
public:
    HttpContentEncodeResult(const HttpContentEncodeResult&) = delete;
    HttpContentEncodeResult& operator=(const HttpContentEncodeResult&) = delete;
    HttpContentEncodeResult(HttpContentEncodeResult&&) noexcept = default;
    HttpContentEncodeResult& operator=(HttpContentEncodeResult&&) = delete;

    [[nodiscard]] HttpEncodedContent* encoded() & noexcept {
        return std::get_if<HttpEncodedContent>(&value_);
    }

    [[nodiscard]] const HttpEncodedContent* encoded() const & noexcept {
        return std::get_if<HttpEncodedContent>(&value_);
    }
    HttpEncodedContent* encoded() && = delete;
    const HttpEncodedContent* encoded() const && = delete;

    [[nodiscard]] const HttpContentEncodeFailure* failure() const & noexcept {
        return std::get_if<HttpContentEncodeFailure>(&value_);
    }
    const HttpContentEncodeFailure* failure() const && = delete;

private:
    friend HttpContentEncodeResult encodeHttpContent(
        HttpContentCoding,
        std::string_view,
        std::size_t,
        std::pmr::memory_resource*);

    using Value = std::variant<HttpEncodedContent, HttpContentEncodeFailure>;

    [[nodiscard]] static HttpContentEncodeResult makeEncoded(
        std::pmr::string bytes) noexcept {
        return HttpContentEncodeResult(HttpEncodedContent(std::move(bytes)));
    }

    [[nodiscard]] static HttpContentEncodeResult makeFailure(
        HttpContentEncodeError error) noexcept {
        return HttpContentEncodeResult(HttpContentEncodeFailure(error));
    }

    explicit HttpContentEncodeResult(HttpEncodedContent encoded) noexcept
        : value_(std::move(encoded)) {}

    explicit HttpContentEncodeResult(
        HttpContentEncodeFailure failure) noexcept
        : value_(failure) {}

    Value value_;
};

// Produces one complete content-coded representation within the exact output
// cap (zero means only a zero-byte encoding could succeed). HTTP zstd output is
// constrained to RFC 9659's 8 MiB window limit.
[[nodiscard]] HttpContentEncodeResult encodeHttpContent(
    HttpContentCoding coding,
    std::string_view input,
    std::size_t maxEncodedBytes,
    std::pmr::memory_resource* resource);

enum class HttpContentDecodeError : std::uint8_t {
    kUnsupportedCoding,
    kInvalidContent,
    kDecodedSizeExceeded,
    kDecoderFailure
};

class HttpDecodedContent final {
public:
    HttpDecodedContent(const HttpDecodedContent&) = delete;
    HttpDecodedContent& operator=(const HttpDecodedContent&) = delete;
    HttpDecodedContent(HttpDecodedContent&&) noexcept = default;
    HttpDecodedContent& operator=(HttpDecodedContent&&) = delete;

    [[nodiscard]] std::string_view bytes() const & noexcept {
        return std::string_view(bytes_.data(), bytes_.size());
    }
    std::string_view bytes() const && = delete;

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

    explicit constexpr HttpContentDecodeFailure(
        HttpContentDecodeError error) noexcept
        : error_(error) {}

    HttpContentDecodeError error_;
};

// Decoding is transactional: success exclusively owns the complete decoded
// bytes, while failure owns only its reason. No caller-provided output buffer
// can expose a partial representation after malformed input or a size failure.
class HttpContentDecodeResult final {
public:
    HttpContentDecodeResult(const HttpContentDecodeResult&) = delete;
    HttpContentDecodeResult& operator=(const HttpContentDecodeResult&) = delete;
    HttpContentDecodeResult(HttpContentDecodeResult&&) noexcept = default;
    HttpContentDecodeResult& operator=(HttpContentDecodeResult&&) = delete;

    [[nodiscard]] HttpDecodedContent* decoded() & noexcept {
        return std::get_if<HttpDecodedContent>(&value_);
    }

    [[nodiscard]] const HttpDecodedContent* decoded() const & noexcept {
        return std::get_if<HttpDecodedContent>(&value_);
    }
    HttpDecodedContent* decoded() && = delete;
    const HttpDecodedContent* decoded() const && = delete;

    [[nodiscard]] const HttpContentDecodeFailure* failure() const & noexcept {
        return std::get_if<HttpContentDecodeFailure>(&value_);
    }
    const HttpContentDecodeFailure* failure() const && = delete;

private:
    friend HttpContentDecodeResult decodeHttpClientResponseContentEncoding(
        const HttpClientResponse&,
        std::size_t,
        std::pmr::memory_resource*);
    friend HttpContentDecodeResult decodeHttpContent(
        HttpContentCoding,
        std::string_view,
        std::size_t,
        std::pmr::memory_resource*);

    using Value = std::variant<HttpDecodedContent, HttpContentDecodeFailure>;

    [[nodiscard]] static HttpContentDecodeResult makeDecoded(
        std::pmr::string bytes) noexcept {
        return HttpContentDecodeResult(HttpDecodedContent(std::move(bytes)));
    }

    [[nodiscard]] static HttpContentDecodeResult makeFailure(
        HttpContentDecodeError error) noexcept {
        return HttpContentDecodeResult(HttpContentDecodeFailure(error));
    }

    explicit HttpContentDecodeResult(HttpDecodedContent decoded) noexcept
        : value_(std::move(decoded)) {}

    explicit HttpContentDecodeResult(
        HttpContentDecodeFailure failure) noexcept
        : value_(failure) {}

    Value value_;
};

// gzip is a series of RFC 1952 members and Zstandard is a sequence of RFC 8878
// frames (with RFC 9659's 8 MiB HTTP window limit), so both are consumed through
// the end of `input`. Brotli defines one stream; any bytes left after its
// terminal meta-block are rejected. The exact decoded-size cap is enforced
// across every member/frame (zero means only an empty decoded representation is
// allowed).
[[nodiscard]] HttpContentDecodeResult decodeHttpContent(
    HttpContentCoding coding,
    std::string_view input,
    std::size_t maxDecodedBytes,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
