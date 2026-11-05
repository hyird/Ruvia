#pragma once

#include <zstd.h>

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "coding/pmr_codec_allocation.h"

// zstd (RFC 8878) state whose working memory comes from one PMR resource and
// whose window never exceeds the 8 MiB HTTP limit of RFC 9659. zstd
// retains the allocator context as its opaque pointer, so these types are
// pinned: owners embed them in storage whose address never changes.
//
// Construction rethrows an allocation failure captured during initialization.
// Any other zstd refusal leaves the stream uninitialized so each owner maps it
// into its own error contract.

namespace ruvia::detail {

class zstd_decoder_stream final {
public:
    explicit zstd_decoder_stream(std::pmr::memory_resource* resource);
    ~zstd_decoder_stream();
    zstd_decoder_stream(const zstd_decoder_stream&) = delete;
    zstd_decoder_stream& operator=(const zstd_decoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return stream_ != nullptr;
    }
    [[nodiscard]] ZSTD_DStream* native() const noexcept {
        return stream_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_allocation_failure();
    }

private:
    pmr_codec_allocation_context allocation_;
    ZSTD_DStream* stream_;
};

// Encoder at the default compression level, with the level's own window.
// reset() ends any frame in progress and keeps both the parameters and the
// working memory, so the next frame is identical to one from a new encoder.
// false means the owner must destroy it.
class zstd_encoder_stream final {
public:
    explicit zstd_encoder_stream(std::pmr::memory_resource* resource);
    ~zstd_encoder_stream();
    zstd_encoder_stream(const zstd_encoder_stream&) = delete;
    zstd_encoder_stream& operator=(const zstd_encoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return context_ != nullptr;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_allocation_failure();
    }
    [[nodiscard]] bool reset() noexcept;
    // The single zstd encode loop, shared by whole-buffer and streaming
    // coding: consumes the input and drains `operation` into output. False
    // means output would exceed `max_encoded_bytes` in total; no byte past
    // that cap is appended. After false or an exception, the owner must reset
    // or discard this stream.
    [[nodiscard]] bool encode(std::string_view input, std::pmr::string& output,
        ZSTD_EndDirective operation, std::size_t max_encoded_bytes);

private:
    pmr_codec_allocation_context allocation_;
    ZSTD_CCtx* context_;
};

}  // namespace ruvia::detail
