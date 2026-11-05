#include "coding/http_brotli_stream.h"

#include <algorithm>
#include <cstdint>
#include <limits>

#include "ruvia/http/http_content_codec.h"

namespace ruvia::detail {

namespace {

inline constexpr std::uint32_t http_brotli_quality = 5;

// Upper bound of the per-metablock Huffman groups (256 trees each of the
// literal, command and 520-symbol distance alphabets at alphabet + 376
// four-byte entries, about 2.7 MB in the reference decoder) plus context maps,
// block-type trees and the state itself. It does not depend on the window.
inline constexpr std::size_t brotli_decoder_table_allowance = 3U * 1024U * 1024U;

// The reference decoder sizes its ring buffer as the smallest power of two
// covering the bytes already decoded plus the declared metablock length, and
// grows it by doubling, holding the old ring during the copy. Content that
// fits `max_decoded_bytes` therefore never needs more than three times that
// cap (1 KiB floor) plus write-ahead slack; a larger ring can only serve
// content the cap rejects anyway.
[[nodiscard]] std::size_t brotli_decoder_native_budget(std::size_t max_decoded_bytes) noexcept {
    constexpr auto unlimited = (std::numeric_limits<std::size_t>::max)();
    constexpr std::size_t ring_floor = 1024;
    constexpr std::size_t ring_slack = 2 * 1024;
    const auto ring_bytes = (std::max)(max_decoded_bytes, ring_floor);
    if (ring_bytes > (unlimited - ring_slack - brotli_decoder_table_allowance) / 3) {
        return unlimited;
    }
    return 3 * ring_bytes + ring_slack + brotli_decoder_table_allowance;
}

}  // namespace

brotli_decoder_stream::brotli_decoder_stream(std::pmr::memory_resource* resource, std::size_t max_decoded_bytes)
    : allocation_(resource, brotli_decoder_native_budget(max_decoded_bytes)),
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

brotli_encoder_stream::brotli_encoder_stream(std::pmr::memory_resource* resource, int window_bits)
    : allocation_(resource),
      state_(BrotliEncoderCreateInstance(&pmr_codec_allocate, &pmr_codec_free, &allocation_)) {
    if (state_ != nullptr &&
        (BrotliEncoderSetParameter(state_, BROTLI_PARAM_QUALITY, http_brotli_quality) != BROTLI_TRUE ||
            BrotliEncoderSetParameter(state_, BROTLI_PARAM_LGWIN, static_cast<std::uint32_t>(window_bits)) != BROTLI_TRUE)) {
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
