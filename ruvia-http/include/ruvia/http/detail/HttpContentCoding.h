#pragma once

#include "ruvia/http/detail/HeaderAcceptUtils.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ruvia::detail {

[[nodiscard]] std::string_view httpContentCodingToken(HttpContentCoding coding) noexcept;

// Maps one complete Content-Encoding field value to the single coding this
// decoder supports directly. Identity, unknown codings, and coding stacks map
// to kNone so their encoded representation remains observable to a caller that
// has not opted into a complete coding stack.
[[nodiscard]] HttpContentCoding httpContentCodingFromFieldValue(
    std::string_view value) noexcept;

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

[[nodiscard]] bool encodeHttpContent(
    HttpContentCoding coding,
    std::string_view input,
    std::pmr::string& output,
    std::size_t maxOutputBytes);

}  // namespace ruvia::detail
