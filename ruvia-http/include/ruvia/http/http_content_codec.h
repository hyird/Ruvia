#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http_content_coding.h"

namespace ruvia {

// A codec that could not run (for example its native state could not be
// created or reset, or the library reported an internal error). It is an
// internal failure, never a property of the representation; encode and decode
// both throw it, and allocation exceptions retain their original type.
class http_content_codec_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct http_content_encode_options final {
    std::size_t max_encoded_bytes_{};
    std::pmr::memory_resource* resource_{nullptr};
};

// Produces one complete content-coded representation within the exact output
// cap. nullopt means only that max_encoded_bytes_ would be exceeded; zero
// permits only a zero-byte encoding. Codec failures throw
// http_content_codec_error.
[[nodiscard]] std::optional<std::pmr::string> encode_http_content(
    http_content_coding coding, std::string_view input, http_content_encode_options options);

// Applies the listed codings in order. Each intermediate representation is
// bounded by max_encoded_bytes and released before the next stage completes.
[[nodiscard]] std::optional<std::pmr::string> encode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_encode_options options);

// Reusable whole-buffer encoder for one coding, for an owner that encodes many
// representations on one thread. The first encode() creates the codec state
// from `state_resource`; gzip, deflate and zstd keep it and reset it after
// every call that returns, and discard it after an exception, so no history
// and no failed state crosses representations. Brotli has no reset, so its
// state is created per call, also from `state_resource`. Results follow
// encode_http_content() exactly: the same bytes, cap, failures, and output
// from options.resource_. The context is not thread-safe, and
// `state_resource` must outlive it.
class http_content_encode_context final {
public:
    http_content_encode_context(http_content_coding coding, std::pmr::memory_resource* state_resource) noexcept;
    ~http_content_encode_context();
    http_content_encode_context(const http_content_encode_context&) = delete;
    http_content_encode_context& operator=(const http_content_encode_context&) = delete;
    http_content_encode_context(http_content_encode_context&&) = delete;
    http_content_encode_context& operator=(http_content_encode_context&&) = delete;

    [[nodiscard]] http_content_coding coding() const noexcept {
        return coding_;
    }

    [[nodiscard]] std::optional<std::pmr::string> encode(
        std::string_view input, http_content_encode_options options);

private:
    struct impl_type;
    http_content_coding coding_;
    std::pmr::memory_resource* resource_;
    impl_type* impl_{nullptr};
};

// Data errors only: a codec that cannot run throws http_content_codec_error.
enum class http_content_decode_error : std::uint8_t {
    unsupported_coding,
    invalid_content,
    decoded_size_exceeded
};

struct http_content_decode_options final {
    std::size_t max_decoded_bytes_{};
    std::pmr::memory_resource* resource_{nullptr};
};

// gzip members and zstd frames are consumed through the end of input; Brotli
// trailing bytes are rejected. The exact decoded-size cap applies across the
// complete representation, and an identity representation over the cap is
// rejected before allocating. An empty representation decodes to empty content
// for every coding. Codec failures throw http_content_codec_error.
[[nodiscard]] std::variant<std::pmr::string, http_content_decode_error> decode_http_content(
    http_content_coding coding, std::string_view input, http_content_decode_options options);

// Decodes the listed codings in reverse order. gzip members and zstd frames are
// consumed through the end of their input; Brotli trailing bytes are rejected.
// An empty coded representation decodes to empty content. The exact
// decoded-size cap applies to every intermediate representation, and each one
// is released before the next stage completes. Codec failures throw
// http_content_codec_error.
[[nodiscard]] std::variant<std::pmr::string, http_content_decode_error> decode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_decode_options options);
}  // namespace ruvia
