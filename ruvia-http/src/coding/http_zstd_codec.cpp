#include <zstd.h>
#include <zstd_errors.h>

#include <cstddef>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/http_content_codec.h"
#include "coding/http_zstd_stream.h"

// zstd (RFC 8878) within the 8 MiB HTTP window limit of RFC 9659.

namespace ruvia::detail {

std::variant<std::pmr::string, http_content_decode_error> decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    std::pmr::string output("", http_pmr_resource_or_default(resource));
    zstd_decoder_stream decoder(output.get_allocator().resource());
    if (!decoder.initialized()) {
        throw http_content_codec_error("zstd decoder initialization failed");
    }
    auto* stream = decoder.native();

    ZSTD_inBuffer in{input.data(), input.size(), 0};
    char buffer[16384];
    for (;;) {
        const auto before_input = in.pos;
        ZSTD_outBuffer out{buffer, sizeof(buffer), 0};
        const auto result_value = ZSTD_decompressStream(stream, &out, &in);
        if (ZSTD_isError(result_value) != 0) {
            decoder.rethrow_allocation_failure();
            if (ZSTD_getErrorCode(result_value) == ZSTD_error_memory_allocation) {
                throw http_content_codec_error(ZSTD_getErrorName(result_value));
            }
            return http_content_decode_error::invalid_content;
        }
        if (!append_decoded_bytes(output, buffer, out.pos, max_decoded_bytes)) {
            return http_content_decode_error::decoded_size_exceeded;
        }
        if (result_value == 0 && in.pos == in.size) {
            return output;
        }
        // A zero result with remaining input completed one RFC 8878 frame;
        // ZSTD_decompressStream is ready to consume the next concatenated frame.
        if (out.pos == 0 && in.pos == before_input) {
            return http_content_decode_error::invalid_content;
        }
    }
}

std::optional<std::pmr::string> encode_zstd_content(
    zstd_encoder_stream& encoder, std::string_view input,
    std::size_t max_encoded_bytes, std::pmr::memory_resource* output_resource) {
    return encode_whole_content_with_stream(encoder, ZSTD_e_end, input, max_encoded_bytes, output_resource);
}

std::optional<std::pmr::string> encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource) {
    return encode_whole_content<zstd_encoder_stream>(
        "zstd encoder initialization failed", ZSTD_e_end, input, max_encoded_bytes, resource);
}

}  // namespace ruvia::detail
