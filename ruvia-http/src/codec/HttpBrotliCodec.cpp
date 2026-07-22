#include "ruvia/http/detail/HttpContentCodec.h"

#include <cstddef>
#include <utility>

#include <brotli/decode.h>
#include <brotli/encode.h>

#include "ruvia/http/detail/PmrResource.h"

// br (RFC 7932) through the Brotli reference library.

namespace ruvia::detail {

namespace {

[[nodiscard]] bool brotliAllocationFailure(
    BrotliDecoderErrorCode error) noexcept {
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

ContentDecodeAttempt decodeBrotliContent(
    std::string_view input,
    std::size_t maxDecodedBytes,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    auto* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (state == nullptr) {
        return HttpContentDecodeError::kDecoderFailure;
    }
    struct Guard final {
        BrotliDecoderState* state;
        ~Guard() { BrotliDecoderDestroyInstance(state); }
    } guard{state};

    const auto* nextInput = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t availableInput = input.size();
    std::uint8_t buffer[16384];
    for (;;) {
        const auto beforeInput = availableInput;
        auto* nextOutput = buffer;
        std::size_t availableOutput = sizeof(buffer);
        const auto result = BrotliDecoderDecompressStream(
            state,
            &availableInput,
            &nextInput,
            &availableOutput,
            &nextOutput,
            nullptr);
        const auto produced = sizeof(buffer) - availableOutput;
        if (!appendDecodedBytes(
                output,
                reinterpret_cast<const char*>(buffer),
                produced,
                maxDecodedBytes)) {
            return HttpContentDecodeError::kDecodedSizeExceeded;
        }
        if (result == BROTLI_DECODER_RESULT_SUCCESS) {
            // RFC 7932 defines one Brotli stream. Its decoder deliberately does
            // not over-consume, so remaining bytes are not part of this content.
            return availableInput == 0
                ? ContentDecodeAttempt(std::move(output))
                : ContentDecodeAttempt(
                      HttpContentDecodeError::kInvalidContent);
        }
        if (result == BROTLI_DECODER_RESULT_ERROR) {
            return brotliAllocationFailure(
                       BrotliDecoderGetErrorCode(state))
                ? HttpContentDecodeError::kDecoderFailure
                : HttpContentDecodeError::kInvalidContent;
        }
        const bool progressed =
            produced != 0 || availableInput != beforeInput;
        if (!progressed ||
            (result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT &&
             availableInput == 0)) {
            return HttpContentDecodeError::kInvalidContent;
        }
    }
}

ContentEncodeAttempt encodeBrotliContent(
    std::string_view input,
    std::size_t maxEncodedBytes,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    auto* state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (state == nullptr) {
        return HttpContentEncodeError::kEncoderFailure;
    }
    struct Guard final {
        BrotliEncoderState* state;
        ~Guard() { BrotliEncoderDestroyInstance(state); }
    } guard{state};
    if (BrotliEncoderSetParameter(
            state,
            BROTLI_PARAM_QUALITY,
            5) != BROTLI_TRUE) {
        return HttpContentEncodeError::kEncoderFailure;
    }

    std::size_t availableInput = input.size();
    const auto* nextInput = reinterpret_cast<const std::uint8_t*>(
        input.data());
    std::uint8_t buffer[8192];
    for (;;) {
        const auto beforeInput = availableInput;
        std::size_t availableOutput = sizeof(buffer);
        auto* nextOutput = buffer;
        if (BrotliEncoderCompressStream(
                state,
                BROTLI_OPERATION_FINISH,
                &availableInput,
                &nextInput,
                &availableOutput,
                &nextOutput,
                nullptr) != BROTLI_TRUE) {
            return HttpContentEncodeError::kEncoderFailure;
        }
        const auto produced = sizeof(buffer) - availableOutput;
        if (output.size() > maxEncodedBytes ||
            produced > maxEncodedBytes - output.size()) {
            return HttpContentEncodeError::kEncodedSizeExceeded;
        }
        output.append(
            reinterpret_cast<const char*>(buffer),
            produced);
        if (BrotliEncoderIsFinished(state) == BROTLI_TRUE) {
            return output;
        }
        if (produced == 0 && availableInput == beforeInput) {
            return HttpContentEncodeError::kEncoderFailure;
        }
    }
}

}  // namespace ruvia::detail
