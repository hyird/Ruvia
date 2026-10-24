#pragma once

#include <cstddef>
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

class http_content_encode_error final : public std::runtime_error {
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
// http_content_encode_error; allocation exceptions retain their original type.
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
// every call, including failures and exceptions, so no history crosses
// representations. Brotli has no reset, so its state is created per call, also
// from `state_resource`. Results follow encode_http_content() exactly: the same
// bytes, cap, failures, and output from options.resource_. The context is not
// thread-safe, and `state_resource` must outlive it.
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
    struct state;
    http_content_coding coding_;
    std::pmr::memory_resource* resource_;
    state* state_{nullptr};
};

// gzip members and zstd frames are consumed through the end of input; Brotli
// trailing bytes are rejected. The exact decoded-size cap applies across the
// complete representation. An empty representation decodes to empty content
// for every coding.
[[nodiscard]] std::variant<std::pmr::string, http_content_decode_error> decode_http_content(
    http_content_coding coding, std::string_view input, http_content_decode_options options);

// Decodes the listed codings in reverse order. Each intermediate representation
// is bounded by max_decoded_bytes and released before the next stage completes.
[[nodiscard]] std::variant<std::pmr::string, http_content_decode_error> decode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_decode_options options);
}  // namespace ruvia
