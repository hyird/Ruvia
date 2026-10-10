#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/http_content_codec.h"

#include "coding/http_content_codec_result.h"

// One compression library binding per coding, behind a uniform signature: decode
// or encode a whole buffer through the caller's memory resource, bounded by an
// explicit output ceiling, reporting failure as a value. Which coding a field
// asks for is decided elsewhere; this is only the machinery each one runs on.
// decode_http_content maps an empty representation to empty content before
// any decoder below runs.

namespace ruvia::detail {

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

[[nodiscard]] http_content_decode_result decode_gzip_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] http_content_decode_result decode_deflate_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] http_content_decode_result decode_brotli_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] http_content_decode_result decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource);

[[nodiscard]] http_content_encode_result encode_gzip_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] http_content_encode_result encode_deflate_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] http_content_encode_result encode_brotli_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource);
[[nodiscard]] http_content_encode_result encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
