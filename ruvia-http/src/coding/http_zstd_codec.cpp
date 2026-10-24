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

std::variant<std::pmr::string, http_content_decode_error> decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    std::pmr::string output(http_pmr_resource_or_default(resource));
    zstd_decoder_stream decoder(output.get_allocator().resource());
    if (!decoder.initialized()) {
        return http_content_decode_error::decoder_failure;
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
            return ZSTD_getErrorCode(result_value) == ZSTD_error_memory_allocation
                       ? http_content_decode_error::decoder_failure
                       : http_content_decode_error::invalid_content;
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
    std::pmr::string output(http_pmr_resource_or_default(output_resource));
    auto* context_value = encoder.native();

    ZSTD_inBuffer in{input.data(), input.size(), 0};
    // Uninitialized scratch: only the bytes zstd produced are copied out.
    char buffer[8192];
    for (;;) {
        if (output.size() == max_encoded_bytes) {
            char probe_value{};
            ZSTD_outBuffer out{&probe_value, 1, 0};
            const auto result_value = ZSTD_compressStream2(context_value, &out, &in, ZSTD_e_end);
            if (ZSTD_isError(result_value) != 0) {
                encoder.rethrow_allocation_failure();
                throw http_content_encode_error(ZSTD_getErrorName(result_value));
            }
            if (result_value == 0 && out.pos == 0 && in.pos == in.size) {
                return output;
            }
            return std::nullopt;
        }

        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(sizeof(buffer), max_encoded_bytes - offset);
        const auto before_input = in.pos;
        ZSTD_outBuffer out{buffer, writable, 0};
        const auto result_value = ZSTD_compressStream2(context_value, &out, &in, ZSTD_e_end);
        output.append(buffer, out.pos);
        if (ZSTD_isError(result_value) != 0) {
            encoder.rethrow_allocation_failure();
            throw http_content_encode_error(ZSTD_getErrorName(result_value));
        }
        if (result_value == 0 && in.pos == in.size) {
            return output;
        }
        if (output.size() == offset && in.pos == before_input) {
            throw http_content_encode_error("zstd encoder made no progress");
        }
    }
}

std::optional<std::pmr::string> encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource) {
    auto* const state_resource = http_pmr_resource_or_default(resource);
    zstd_encoder_stream encoder(state_resource);
    if (!encoder.initialized()) {
        throw http_content_encode_error("zstd encoder initialization failed");
    }
    return encode_zstd_content(encoder, input, max_encoded_bytes, state_resource);
}

}  // namespace ruvia::detail
