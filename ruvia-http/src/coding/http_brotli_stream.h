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

class brotli_decoder_stream final {
public:
    explicit brotli_decoder_stream(std::pmr::memory_resource* resource);
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

private:
    pmr_codec_allocation_context allocation_;
    BrotliDecoderState* state_;
};

// Encoder at quality 5, shared by whole-buffer and streaming HTTP content coding.
class brotli_encoder_stream final {
public:
    explicit brotli_encoder_stream(std::pmr::memory_resource* resource);
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
