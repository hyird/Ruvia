#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_content_codec.h"

// One compression library binding per coding, behind a uniform signature: decode
// or encode a whole buffer, bounded by an
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
    std::string& output, const char* bytes_value, std::size_t size, std::size_t max_decoded_bytes) {
    if (output.size() > max_decoded_bytes || size > max_decoded_bytes - output.size()) {
        return false;
    }
    output.append(bytes_value, size);
    return true;
}

template <typename stream_type, typename operation_type>
[[nodiscard]] inline std::optional<std::string> encode_whole_content_with_stream(
    stream_type& encoder, operation_type finish_operation, std::string_view input,
    std::size_t max_encoded_bytes) {
    std::string output;
    if (!encoder.encode(input, output, finish_operation, max_encoded_bytes)) {
        return std::nullopt;
    }
    return output;
}

template <typename stream_type, typename operation_type, typename... arg_types>
[[nodiscard]] inline std::optional<std::string> encode_whole_content(
    const char* init_error, operation_type finish_operation, std::string_view input,
    std::size_t max_encoded_bytes, arg_types&&... args) {
    stream_type encoder(std::forward<arg_types>(args)...);
    if (!encoder.initialized()) {
        throw http_content_codec_error(init_error);
    }
    return encode_whole_content_with_stream(
        encoder, finish_operation, input, max_encoded_bytes);
}

[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_gzip_content(
    std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_deflate_content(
    std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_brotli_content(
    std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes);

[[nodiscard]] std::optional<std::string> encode_gzip_content(
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_deflate_content(
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_brotli_content(
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes);

// A whole-buffer encode, one finishing pass of the stream's single encode
// loop, over caller-owned codec state that is freshly initialized or reset.
// Every path leaves the state consumed (finished or mid-stream), so an owner
// that reuses it resets it afterwards.
[[nodiscard]] std::optional<std::string> encode_zlib_content(zlib_deflate_stream& deflater,
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_brotli_content(brotli_encoder_stream& encoder,
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_zstd_content(zstd_encoder_stream& encoder,
    std::string_view input, std::size_t max_encoded_bytes);
}  // namespace ruvia::detail
