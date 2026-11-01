#include "coding/http_brotli_stream.h"

#include <cstdint>

#include "ruvia/http/http_content_codec.h"

namespace ruvia::detail {

namespace {

inline constexpr std::uint32_t http_brotli_quality = 5;

}  // namespace

brotli_decoder_stream::brotli_decoder_stream(std::pmr::memory_resource* resource)
    : allocation_(resource),
      state_(BrotliDecoderCreateInstance(&pmr_codec_allocate, &pmr_codec_free, &allocation_)) {
    if (state_ == nullptr) {
        allocation_.rethrow_allocation_failure();
    }
}

brotli_decoder_stream::~brotli_decoder_stream() {
    if (state_ != nullptr) {
        BrotliDecoderDestroyInstance(state_);
    }
}

brotli_encoder_stream::brotli_encoder_stream(std::pmr::memory_resource* resource)
    : allocation_(resource),
      state_(BrotliEncoderCreateInstance(&pmr_codec_allocate, &pmr_codec_free, &allocation_)) {
    if (state_ != nullptr && BrotliEncoderSetParameter(state_, BROTLI_PARAM_QUALITY, http_brotli_quality) != BROTLI_TRUE) {
        BrotliEncoderDestroyInstance(state_);
        state_ = nullptr;
    }
    if (state_ == nullptr) {
        allocation_.rethrow_allocation_failure();
    }
}

brotli_encoder_stream::~brotli_encoder_stream() {
    if (state_ != nullptr) {
        BrotliEncoderDestroyInstance(state_);
    }
}

bool brotli_encoder_stream::encode(std::string_view input, std::pmr::string& output,
    BrotliEncoderOperation operation, std::size_t max_encoded_bytes) {
    std::size_t available_input = input.size();
    const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
    for (;;) {
        const auto before_input = available_input;
        std::size_t available_output = 0;
        const auto compressed = BrotliEncoderCompressStream(state_, operation,
            &available_input, &next_input, &available_output, nullptr, nullptr);
        allocation_.rethrow_allocation_failure();
        if (compressed != BROTLI_TRUE) {
            throw http_content_codec_error("brotli encoding failed");
        }
        std::size_t produced = 0;
        const auto* bytes_value = BrotliEncoderTakeOutput(state_, &produced);
        if (output.size() > max_encoded_bytes || produced > max_encoded_bytes - output.size()) {
            return false;
        }
        // Consume the borrowed block directly into the caller's output before
        // any subsequent encoder call can invalidate it.
        if (produced != 0) {
            output.append(reinterpret_cast<const char*>(bytes_value), produced);
        }
        if (operation == BROTLI_OPERATION_FINISH) {
            if (BrotliEncoderIsFinished(state_) == BROTLI_TRUE) {
                return true;
            }
        } else if (available_input == 0 && BrotliEncoderHasMoreOutput(state_) != BROTLI_TRUE) {
            return true;
        }
        if (produced == 0 && available_input == before_input) {
            throw http_content_codec_error("brotli encoder made no progress");
        }
    }
}

}  // namespace ruvia::detail
