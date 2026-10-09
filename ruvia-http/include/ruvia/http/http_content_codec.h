#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http_content_coding.h"

namespace ruvia {

namespace detail {
struct http_content_decode_result_access;
struct http_content_encode_result_access;
}  // namespace detail

enum class http_content_encode_error : std::uint8_t { encoded_size_exceeded,
    encoder_failure };

struct http_content_encode_options final {
    std::size_t max_encoded_bytes_{};
    std::pmr::memory_resource* resource_{nullptr};
};

class http_encoded_content final {
public:
    http_encoded_content(const http_encoded_content&) = delete;
    http_encoded_content& operator=(const http_encoded_content&) = delete;
    http_encoded_content(http_encoded_content&&) noexcept = default;
    http_encoded_content& operator=(http_encoded_content&&) = delete;

    [[nodiscard]] std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

    [[nodiscard]] std::pmr::string take_bytes() && noexcept {
        return std::move(bytes_);
    }

private:
    friend class http_content_encode_result;
    friend struct detail::http_content_encode_result_access;

    explicit http_encoded_content(std::pmr::string bytes_value) noexcept
        : bytes_(std::move(bytes_value)) {}

    std::pmr::string bytes_;
};

class http_content_encode_failure final {
public:
    [[nodiscard]] constexpr http_content_encode_error error() const noexcept {
        return error_;
    }

private:
    friend class http_content_encode_result;
    friend struct detail::http_content_encode_result_access;

    explicit constexpr http_content_encode_failure(http_content_encode_error error) noexcept
        : error_(error) {}

    http_content_encode_error error_;
};

// Encoding is transactional: success exclusively owns the complete encoded
// representation, while failure owns only its reason.
class http_content_encode_result final {
public:
    http_content_encode_result(const http_content_encode_result&) = delete;
    http_content_encode_result& operator=(const http_content_encode_result&) = delete;
    http_content_encode_result(http_content_encode_result&&) noexcept = default;
    http_content_encode_result& operator=(http_content_encode_result&&) = delete;

    [[nodiscard]] http_encoded_content* encoded() & noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }

    [[nodiscard]] const http_encoded_content* encoded() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    http_encoded_content* encoded() && = delete;
    const http_encoded_content* encoded() const&& = delete;

    [[nodiscard]] const http_content_encode_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http_content_encode_failure* failure() const&& = delete;

private:
    friend struct detail::http_content_encode_result_access;

    using value_type = std::variant<http_encoded_content, http_content_encode_failure>;

    explicit http_content_encode_result(http_encoded_content encoded) noexcept
        : value_(std::move(encoded)) {}

    explicit http_content_encode_result(http_content_encode_failure failure) noexcept
        : value_(failure) {}

    value_type value_;
};

// Produces one complete content-coded representation within the exact output
// cap. Zero permits only a zero-byte encoding.
[[nodiscard]] http_content_encode_result encode_http_content(
    http_content_coding coding, std::string_view input, http_content_encode_options options);

// Applies the listed codings in order. Each intermediate representation is
// bounded by max_encoded_bytes and released before the next stage completes.
[[nodiscard]] http_content_encode_result encode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_encode_options options);

enum class http_content_decode_error : std::uint8_t {
    unsupported_coding,
    invalid_content,
    decoded_size_exceeded,
    decoder_failure
};

struct http_content_decode_options final {
    std::size_t max_decoded_bytes_{};
    std::pmr::memory_resource* resource_{nullptr};
};

class http_decoded_content final {
public:
    http_decoded_content(const http_decoded_content&) = delete;
    http_decoded_content& operator=(const http_decoded_content&) = delete;
    http_decoded_content(http_decoded_content&&) noexcept = default;
    http_decoded_content& operator=(http_decoded_content&&) = delete;

    [[nodiscard]] std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

    [[nodiscard]] std::pmr::string take_bytes() && noexcept {
        return std::move(bytes_);
    }

private:
    friend class http_content_decode_result;
    friend struct detail::http_content_decode_result_access;

    explicit http_decoded_content(std::pmr::string bytes_value) noexcept
        : bytes_(std::move(bytes_value)) {}

    std::pmr::string bytes_;
};

class http_content_decode_failure final {
public:
    [[nodiscard]] constexpr http_content_decode_error error() const noexcept {
        return error_;
    }

private:
    friend class http_content_decode_result;
    friend struct detail::http_content_decode_result_access;

    explicit constexpr http_content_decode_failure(http_content_decode_error error) noexcept
        : error_(error) {}

    http_content_decode_error error_;
};

// Decoding is transactional: success exclusively owns the complete decoded
// bytes, while malformed input and size failures expose no partial output.
class http_content_decode_result final {
public:
    http_content_decode_result(const http_content_decode_result&) = delete;
    http_content_decode_result& operator=(const http_content_decode_result&) = delete;
    http_content_decode_result(http_content_decode_result&&) noexcept = default;
    http_content_decode_result& operator=(http_content_decode_result&&) = delete;

    [[nodiscard]] http_decoded_content* decoded() & noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }

    [[nodiscard]] const http_decoded_content* decoded() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    http_decoded_content* decoded() && = delete;
    const http_decoded_content* decoded() const&& = delete;

    [[nodiscard]] const http_content_decode_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http_content_decode_failure* failure() const&& = delete;

private:
    friend struct detail::http_content_decode_result_access;

    using value_type = std::variant<http_decoded_content, http_content_decode_failure>;

    explicit http_content_decode_result(http_decoded_content decoded) noexcept
        : value_(std::move(decoded)) {}

    explicit http_content_decode_result(http_content_decode_failure failure) noexcept
        : value_(failure) {}

    value_type value_;
};

// gzip members and zstd frames are consumed through the end of input; Brotli
// trailing bytes are rejected. The exact decoded-size cap applies across the
// complete representation.
[[nodiscard]] http_content_decode_result decode_http_content(
    http_content_coding coding, std::string_view input, http_content_decode_options options);

// Decodes the listed codings in reverse order. Each intermediate representation
// is bounded by max_decoded_bytes and released before the next stage completes.
[[nodiscard]] http_content_decode_result decode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_decode_options options);

}  // namespace ruvia
