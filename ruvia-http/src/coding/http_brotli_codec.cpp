#include <brotli/decode.h>
#include <brotli/encode.h>

#include <cstddef>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/http_content_codec.h"
#include "coding/pmr_codec_allocation.h"

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

http_content_decode_result decode_brotli_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    std::pmr::string output(http_pmr_resource_or_default(resource));
    pmr_codec_allocation_context allocation_context(output.get_allocator().resource());
    auto* state_value = BrotliDecoderCreateInstance(
        &pmr_codec_allocate_with_exception, &pmr_codec_free, &allocation_context);
    if (state_value == nullptr) {
        allocation_context.rethrow_allocation_failure();
        return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
    }
    struct guard final {
        BrotliDecoderState* state_;
        ~guard() {
            BrotliDecoderDestroyInstance(state_);
        }
    } guard_value{state_value};
    allocation_context.rethrow_allocation_failure();

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
                return http_content_decode_result_access::failure(http_content_decode_error::decoded_size_exceeded);
            }
            produced_output = produced_output || produced != 0;
        }
        if (result_value == BROTLI_DECODER_RESULT_SUCCESS) {
            // RFC 7932 defines one Brotli stream. Its decoder deliberately does
            // not over-consume, so remaining bytes are not part of this content.
            if (available_input == 0) {
                return http_content_decode_result_access::decoded(std::move(output));
            }
            return http_content_decode_result_access::failure(http_content_decode_error::invalid_content);
        }
        if (result_value == BROTLI_DECODER_RESULT_ERROR) {
            allocation_context.rethrow_allocation_failure();
            return http_content_decode_result_access::failure(brotli_allocation_failure(BrotliDecoderGetErrorCode(state_value))
                                                                  ? http_content_decode_error::decoder_failure
                                                                  : http_content_decode_error::invalid_content);
        }
        const bool progressed = produced_output || available_input != before_input;
        if (!progressed ||
            (result_value == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && available_input == 0)) {
            return http_content_decode_result_access::failure(http_content_decode_error::invalid_content);
        }
    }
}

http_content_encode_result encode_brotli_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource) {
    std::pmr::string output(http_pmr_resource_or_default(resource));
    pmr_codec_allocation_context allocation_context(output.get_allocator().resource());
    auto* state_value = BrotliEncoderCreateInstance(
        &pmr_codec_allocate_with_exception, &pmr_codec_free, &allocation_context);
    if (state_value == nullptr) {
        allocation_context.rethrow_allocation_failure();
        return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
    }
    struct guard final {
        BrotliEncoderState* state_;
        ~guard() {
            BrotliEncoderDestroyInstance(state_);
        }
    } guard_value{state_value};
    allocation_context.rethrow_allocation_failure();
    const auto configured = BrotliEncoderSetParameter(state_value, BROTLI_PARAM_QUALITY, 5);
    allocation_context.rethrow_allocation_failure();
    if (configured != BROTLI_TRUE) {
        return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
    }

    std::size_t available_input = input.size();
    const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
    for (;;) {
        const auto before_input = available_input;
        std::size_t available_output = 0;
        const auto compressed = BrotliEncoderCompressStream(state_value, BROTLI_OPERATION_FINISH,
            &available_input, &next_input, &available_output, nullptr, nullptr);
        allocation_context.rethrow_allocation_failure();
        if (compressed != BROTLI_TRUE) {
            allocation_context.rethrow_allocation_failure();
            return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
        }
        std::size_t produced = 0;
        const auto* bytes_value = BrotliEncoderTakeOutput(state_value, &produced);
        if (output.size() > max_encoded_bytes || produced > max_encoded_bytes - output.size()) {
            return http_content_encode_result_access::failure(http_content_encode_error::encoded_size_exceeded);
        }
        // The borrowed block expires on the next encoder call. Copy it directly
        // into the result before checking or advancing the encoder state.
        if (produced != 0) {
            output.append(reinterpret_cast<const char*>(bytes_value), produced);
        }
        if (BrotliEncoderIsFinished(state_value) == BROTLI_TRUE) {
            return http_content_encode_result_access::encoded(std::move(output));
        }
        if (produced == 0 && available_input == before_input) {
            return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
        }
    }
}

}  // namespace ruvia::detail
