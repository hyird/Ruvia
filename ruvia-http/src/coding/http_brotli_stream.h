#pragma once

#include <brotli/decode.h>
#include <brotli/encode.h>

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "coding/pmr_codec_allocation.h"

// Brotli (RFC 7932) state whose working memory comes from one PMR resource.
// Brotli retains the allocator context as its opaque pointer, so these types are
// pinned: owners embed them in storage whose address never changes.
//
// Construction rethrows an allocation failure captured during initialization.
// Any other Brotli refusal leaves the stream uninitialized so each owner maps it
// into its own error contract.

namespace ruvia::detail {

// Decoder whose native working memory is bounded by the caller's decoded-size
// cap rather than by the window and metablock sizes the peer declares: a
// declared ring buffer that the cap could never fill is refused, and
// native_budget_exceeded() then reports a cap violation instead of a codec
// failure.
class brotli_decoder_stream final {
public:
    brotli_decoder_stream(std::pmr::memory_resource* resource, std::size_t max_decoded_bytes);
    ~brotli_decoder_stream();
    brotli_decoder_stream(const brotli_decoder_stream&) = delete;
    brotli_decoder_stream& operator=(const brotli_decoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return state_ != nullptr;
    }
    [[nodiscard]] BrotliDecoderState* native() const noexcept {
        return state_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_allocation_failure();
    }
    [[nodiscard]] bool native_budget_exceeded() const noexcept {
        return allocation_.budget_exceeded();
    }

private:
    pmr_codec_allocation_context allocation_;
    BrotliDecoderState* state_;
};

// LZ77 window exponents. Whole-buffer encoding is bounded by its input and
// released at once, so it keeps the library default; a stream encoder lives as
// long as its response (SSE), so it uses Pingora's 512 KiB window.
inline constexpr int brotli_whole_buffer_window_bits = BROTLI_DEFAULT_WINDOW;
inline constexpr int brotli_stream_window_bits = 19;

// Encoder at quality 5, shared by whole-buffer and streaming HTTP content coding.
class brotli_encoder_stream final {
public:
    brotli_encoder_stream(std::pmr::memory_resource* resource, int window_bits);
    ~brotli_encoder_stream();
    brotli_encoder_stream(const brotli_encoder_stream&) = delete;
    brotli_encoder_stream& operator=(const brotli_encoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return state_ != nullptr;
    }
    // Consumes the input and drains the requested operation into output. False
    // means the total output budget was exceeded before appending a block.
    // After false or an exception, the owner must discard/rebuild this stream.
    [[nodiscard]] bool encode(std::string_view input, std::pmr::string& output,
        BrotliEncoderOperation operation, std::size_t max_encoded_bytes);

private:
    pmr_codec_allocation_context allocation_;
    BrotliEncoderState* state_;
};

}  // namespace ruvia::detail
