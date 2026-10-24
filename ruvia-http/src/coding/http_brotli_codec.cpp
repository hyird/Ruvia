#include <brotli/decode.h>
#include <brotli/encode.h>

#include <cstddef>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/http_brotli_stream.h"
#include "coding/http_content_codec.h"

// br (RFC 7932) through the Brotli reference library.

namespace ruvia::detail {

namespace {

[[nodiscard]] bool brotli_allocation_failure(BrotliDecoderErrorCode error) noexcept {
    switch (error) {
        case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES:
        case BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS:
        case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP:
        case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1:
        case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2:
        case BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES:
            return true;
        default:
            return false;
    }
}

}  // namespace

std::variant<std::pmr::string, http_content_decode_error> decode_brotli_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    std::pmr::string output("", http_pmr_resource_or_default(resource));
    brotli_decoder_stream decoder(output.get_allocator().resource());
    if (!decoder.initialized()) {
        return http_content_decode_error::decoder_failure;
    }
    auto* state_value = decoder.native();

    const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t available_input = input.size();
    for (;;) {
        const auto before_input = available_input;
        std::size_t available_output = 0;
        const auto result_value = BrotliDecoderDecompressStream(
            state_value, &available_input, &next_input, &available_output, nullptr, nullptr);
        bool produced_output = false;
        // The ring buffer can expose more than one contiguous block. Consume
        // each borrow before the next decoder call invalidates it.
        while (BrotliDecoderHasMoreOutput(state_value) == BROTLI_TRUE) {
            std::size_t produced = 0;
            const auto* bytes_value = BrotliDecoderTakeOutput(state_value, &produced);
            if (!append_decoded_bytes(
                    output, reinterpret_cast<const char*>(bytes_value), produced, max_decoded_bytes)) {
                return http_content_decode_error::decoded_size_exceeded;
            }
            produced_output = produced_output || produced != 0;
        }
        if (result_value == BROTLI_DECODER_RESULT_SUCCESS) {
            // RFC 7932 defines one Brotli stream. Its decoder deliberately does
            // not over-consume, so remaining bytes are not part of this content.
            if (available_input == 0) {
                return output;
            }
            return http_content_decode_error::invalid_content;
        }
        if (result_value == BROTLI_DECODER_RESULT_ERROR) {
            decoder.rethrow_allocation_failure();
            return brotli_allocation_failure(BrotliDecoderGetErrorCode(state_value))
                       ? http_content_decode_error::decoder_failure
                       : http_content_decode_error::invalid_content;
        }
        const bool progressed = produced_output || available_input != before_input;
        if (!progressed ||
            (result_value == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && available_input == 0)) {
            return http_content_decode_error::invalid_content;
        }
    }
}

std::optional<std::pmr::string> encode_brotli_content(
    brotli_encoder_stream& encoder, std::string_view input,
    std::size_t max_encoded_bytes, std::pmr::memory_resource* output_resource) {
    std::pmr::string output("", http_pmr_resource_or_default(output_resource));
    auto* state_value = encoder.native();

    std::size_t available_input = input.size();
    const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
    for (;;) {
        const auto before_input = available_input;
        std::size_t available_output = 0;
        const auto compressed = BrotliEncoderCompressStream(state_value, BROTLI_OPERATION_FINISH,
            &available_input, &next_input, &available_output, nullptr, nullptr);
        encoder.rethrow_allocation_failure();
        if (compressed != BROTLI_TRUE) {
            throw http_content_encode_error("brotli encoding failed");
        }
        std::size_t produced = 0;
        const auto* bytes_value = BrotliEncoderTakeOutput(state_value, &produced);
        if (output.size() > max_encoded_bytes || produced > max_encoded_bytes - output.size()) {
            return std::nullopt;
        }
        // The borrowed block expires on the next encoder call. Copy it directly
        // into the result before checking or advancing the encoder state.
        if (produced != 0) {
            output.append(reinterpret_cast<const char*>(bytes_value), produced);
        }
        if (BrotliEncoderIsFinished(state_value) == BROTLI_TRUE) {
            return output;
        }
        if (produced == 0 && available_input == before_input) {
            throw http_content_encode_error("brotli encoder made no progress");
        }
    }
}

std::optional<std::pmr::string> encode_brotli_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource) {
    auto* const state_resource = http_pmr_resource_or_default(resource);
    brotli_encoder_stream encoder(state_resource);
    if (!encoder.initialized()) {
        throw http_content_encode_error("brotli encoder initialization failed");
    }
    return encode_brotli_content(encoder, input, max_encoded_bytes, state_resource);
}

}  // namespace ruvia::detail
