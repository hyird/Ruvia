#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_content_codec.h"

// One compression library binding per coding, behind a uniform signature: decode
// or encode a whole buffer through the caller's memory resource, bounded by an
// explicit output ceiling. Decode reports data errors as values; encode returns
// nullopt only for the ceiling. Both throw http_content_codec_error when the
// codec itself cannot run. Which coding a field asks for is decided elsewhere;
// this is only the machinery each one runs on. decode_http_content maps an
// empty representation to empty content before any decoder below runs.

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

template <typename stream_type, typename operation_type>
[[nodiscard]] inline std::optional<std::pmr::string> encode_whole_content_with_stream(
    stream_type& encoder, operation_type finish_operation, std::string_view input,
    std::size_t max_encoded_bytes, std::pmr::memory_resource* output_resource) {
    std::pmr::string output("", http_pmr_resource_or_default(output_resource));
    if (!encoder.encode(input, output, finish_operation, max_encoded_bytes)) {
        return std::nullopt;
    }
    return output;
}

template <typename stream_type, typename operation_type, typename... arg_types>
[[nodiscard]] inline std::optional<std::pmr::string> encode_whole_content(
    const char* init_error, operation_type finish_operation, std::string_view input,
    std::size_t max_encoded_bytes, std::pmr::memory_resource* resource, arg_types&&... args) {
    auto* const state_resource = http_pmr_resource_or_default(resource);
    stream_type encoder(state_resource, std::forward<arg_types>(args)...);
    if (!encoder.initialized()) {
        throw http_content_codec_error(init_error);
    }
    return encode_whole_content_with_stream(
        encoder, finish_operation, input, max_encoded_bytes, state_resource);
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

// A whole-buffer encode, one finishing pass of the stream's single encode
// loop, over caller-owned codec state that is freshly initialized or reset.
// Output comes from `output_resource`. Every path leaves the state consumed
// (finished or mid-stream), so an owner that reuses it resets it afterwards.
[[nodiscard]] std::optional<std::pmr::string> encode_zlib_content(zlib_deflate_stream& deflater,
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* output_resource);
[[nodiscard]] std::optional<std::pmr::string> encode_brotli_content(brotli_encoder_stream& encoder,
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* output_resource);
[[nodiscard]] std::optional<std::pmr::string> encode_zstd_content(zstd_encoder_stream& encoder,
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* output_resource);
}  // namespace ruvia::detail
