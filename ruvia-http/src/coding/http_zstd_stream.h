#pragma once

#include <zstd.h>

#include <memory_resource>

#include "coding/pmr_codec_allocation.h"

// zstd (RFC 8878) state whose working memory comes from one PMR resource and
// whose window is bounded by the mandatory 8 MiB HTTP limit of RFC 9659. zstd
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

// Encoder at the default compression level. reset() ends any frame in progress
// and keeps both the parameters and the working memory, so the next frame is
// identical to one from a new encoder. false means the owner must destroy it.
class zstd_encoder_stream final {
public:
    explicit zstd_encoder_stream(std::pmr::memory_resource* resource);
    ~zstd_encoder_stream();
    zstd_encoder_stream(const zstd_encoder_stream&) = delete;
    zstd_encoder_stream& operator=(const zstd_encoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return context_ != nullptr;
    }
    [[nodiscard]] ZSTD_CCtx* native() const noexcept {
        return context_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_allocation_failure();
    }
    [[nodiscard]] bool reset() noexcept;

private:
    pmr_codec_allocation_context allocation_;
    ZSTD_CCtx* context_;
};

}  // namespace ruvia::detail
