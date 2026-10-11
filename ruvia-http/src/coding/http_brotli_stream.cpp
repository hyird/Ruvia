#include "coding/http_brotli_stream.h"

#include <cstdint>

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

}  // namespace ruvia::detail
