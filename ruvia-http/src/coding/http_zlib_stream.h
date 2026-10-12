#pragma once

#include <zlib.h>

#include <memory_resource>

#include "coding/pmr_codec_allocation.h"

// zlib inflate/deflate state whose working memory, including the LZ77 window,
// comes from one PMR resource. zlib keeps back-pointers to both the z_stream and
// the allocator context, so these types are pinned: owners embed them in storage
// whose address never changes.
//
// Construction rethrows an allocation failure captured during initialization.
// Any other zlib refusal leaves the stream uninitialized so each owner maps it
// into its own error contract.
//
// reset() returns an initialized stream to its freshly initialized state while
// keeping its working memory, so the next unit of work produces the same bytes
// as a new stream. false means the native state is unusable and the owner must
// destroy the stream.

namespace ruvia::detail {

// zlib's own default; deflate memory is about (1 << (window_bits + 2)) +
// (1 << (mem_level + 9)) bytes, plus the zlib >= 1.2.12 pending buffer.
inline constexpr int zlib_default_mem_level = 8;

class zlib_inflate_stream final {
public:
    zlib_inflate_stream(std::pmr::memory_resource* resource, int window_bits);
    ~zlib_inflate_stream();
    zlib_inflate_stream(const zlib_inflate_stream&) = delete;
    zlib_inflate_stream& operator=(const zlib_inflate_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return stream_.state != nullptr;
    }
    [[nodiscard]] z_stream& native() noexcept {
        return stream_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_allocation_failure();
    }
    [[nodiscard]] bool reset() noexcept;

private:
    pmr_codec_allocation_context allocation_;
    z_stream stream_;
};

class zlib_deflate_stream final {
public:
    zlib_deflate_stream(
        std::pmr::memory_resource* resource, int level, int window_bits, int mem_level, int strategy);
    ~zlib_deflate_stream();
    zlib_deflate_stream(const zlib_deflate_stream&) = delete;
    zlib_deflate_stream& operator=(const zlib_deflate_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return stream_.state != nullptr;
    }
    [[nodiscard]] z_stream& native() noexcept {
        return stream_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_allocation_failure();
    }
    [[nodiscard]] bool reset() noexcept;

private:
    pmr_codec_allocation_context allocation_;
    z_stream stream_;
};

}  // namespace ruvia::detail
