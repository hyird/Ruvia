#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/http_content_codec.h"

// One compression library binding per coding, behind a uniform signature: decode
// or encode a whole buffer through the caller's memory resource, bounded by an
// explicit output ceiling. Decode reports failures as values; encode returns
// nullopt only for the ceiling and throws for codec failures. Which coding a field
// asks for is decided elsewhere; this is only the machinery each one runs on.
// decode_http_content maps an empty representation to empty content before
// any decoder below runs.

namespace ruvia::detail {

class brotli_encoder_stream;
class zlib_deflate_stream;
class zstd_encoder_stream;

// Append decoder output while enforcing the ceiling; false means the ceiling
// would be exceeded and the decode must fail.
[[nodiscard]] inline bool append_decoded_bytes(
    std::pmr::string& output, const char* bytes_value, std::size_t size, std::size_t max_decoded_bytes) {
    if (output.size() > max_decoded_bytes || size > max_decoded_bytes - output.size()) {
        return false;
    }
    output.append(bytes_value, size);
    return true;
}

[[nodiscard]] std::variant<std::pmr::string, http_content_decode_error> decode_gzip_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] std::variant<std::pmr::string, http_content_decode_error> decode_deflate_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] std::variant<std::pmr::string, http_content_decode_error> decode_brotli_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] std::variant<std::pmr::string, http_content_decode_error> decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource);

[[nodiscard]] std::optional<std::pmr::string> encode_gzip_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] std::optional<std::pmr::string> encode_deflate_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] std::optional<std::pmr::string> encode_brotli_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] std::optional<std::pmr::string> encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource);

// The single whole-buffer encode loop of each library, over caller-owned codec
// state that is freshly initialized or reset. Output comes from
// `output_resource`. Every path leaves the state consumed (finished or
// mid-stream), so an owner that reuses it resets it afterwards.
[[nodiscard]] std::optional<std::pmr::string> encode_zlib_content(zlib_deflate_stream& deflater,
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* output_resource);
[[nodiscard]] std::optional<std::pmr::string> encode_brotli_content(brotli_encoder_stream& encoder,
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* output_resource);
[[nodiscard]] std::optional<std::pmr::string> encode_zstd_content(zstd_encoder_stream& encoder,
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* output_resource);
}  // namespace ruvia::detail
