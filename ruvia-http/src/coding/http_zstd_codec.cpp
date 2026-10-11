#include <zstd.h>
#include <zstd_errors.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/http_content_codec.h"
#include "coding/http_zstd_stream.h"

// zstd (RFC 8878) with the mandatory 8 MiB HTTP window limit of RFC 9659.

namespace ruvia::detail {

http_content_decode_result decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    std::pmr::string output(http_pmr_resource_or_default(resource));
    zstd_decoder_stream decoder(output.get_allocator().resource());
    if (!decoder.initialized()) {
        return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
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
            return http_content_decode_result_access::failure(ZSTD_getErrorCode(result_value) == ZSTD_error_memory_allocation
                                                                  ? http_content_decode_error::decoder_failure
                                                                  : http_content_decode_error::invalid_content);
        }
        if (!append_decoded_bytes(output, buffer, out.pos, max_decoded_bytes)) {
            return http_content_decode_result_access::failure(http_content_decode_error::decoded_size_exceeded);
        }
        if (result_value == 0 && in.pos == in.size) {
            return http_content_decode_result_access::decoded(std::move(output));
        }
        // A zero result with remaining input completed one RFC 8878 frame;
        // ZSTD_decompressStream is ready to consume the next concatenated frame.
        if (out.pos == 0 && in.pos == before_input) {
            return http_content_decode_result_access::failure(http_content_decode_error::invalid_content);
        }
    }
}

http_content_encode_result encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource) {
    std::pmr::string output(http_pmr_resource_or_default(resource));
    zstd_encoder_stream encoder(output.get_allocator().resource());
    if (!encoder.initialized()) {
        return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
    }
    auto* context_value = encoder.native();

    ZSTD_inBuffer in{input.data(), input.size(), 0};
    for (;;) {
        if (output.size() == max_encoded_bytes) {
            char probe_value{};
            ZSTD_outBuffer out{&probe_value, 1, 0};
            const auto result_value = ZSTD_compressStream2(context_value, &out, &in, ZSTD_e_end);
            if (ZSTD_isError(result_value) != 0) {
                encoder.rethrow_allocation_failure();
                return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
            }
            if (result_value == 0 && out.pos == 0 && in.pos == in.size) {
                return http_content_encode_result_access::encoded(std::move(output));
            }
            return http_content_encode_result_access::failure(http_content_encode_error::encoded_size_exceeded);
        }

        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(8192, max_encoded_bytes - offset);
        const auto before_input = in.pos;
        std::size_t result_value = 0;
        output.resize(offset + writable);
        ZSTD_outBuffer out{output.data() + offset, writable, 0};
        result_value = ZSTD_compressStream2(context_value, &out, &in, ZSTD_e_end);
        output.resize(offset + out.pos);
        if (ZSTD_isError(result_value) != 0) {
            encoder.rethrow_allocation_failure();
            return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
        }
        if (result_value == 0 && in.pos == in.size) {
            return http_content_encode_result_access::encoded(std::move(output));
        }
        if (output.size() == offset && in.pos == before_input) {
            return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
        }
    }
}

}  // namespace ruvia::detail
