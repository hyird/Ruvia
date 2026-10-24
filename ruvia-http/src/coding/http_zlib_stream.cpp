#include "coding/http_zlib_stream.h"

#include <cstddef>
#include <limits>

namespace ruvia::detail {

namespace {

// zlib requests `items * size` bytes. Zero-sized and overflowing requests are
// refused with nullptr, which is how zlib expects allocation failure.
voidpf zlib_allocate(voidpf opaque, uInt items, uInt size) noexcept {
    if (items == 0 || size == 0) {
        return nullptr;
    }
    const auto item_bytes = static_cast<std::size_t>(items);
    const auto size_bytes = static_cast<std::size_t>(size);
    if (item_bytes > (std::numeric_limits<std::size_t>::max)() / size_bytes) {
        return nullptr;
    }
    return pmr_codec_allocate(opaque, item_bytes * size_bytes);
}

[[nodiscard]] z_stream make_zlib_stream(pmr_codec_allocation_context& allocation) noexcept {
    z_stream stream{};
    stream.zalloc = &zlib_allocate;
    stream.zfree = &pmr_codec_free;
    stream.opaque = &allocation;
    return stream;
}

}  // namespace

zlib_inflate_stream::zlib_inflate_stream(std::pmr::memory_resource* resource, int window_bits)
    : allocation_(resource),
      stream_(make_zlib_stream(allocation_)) {
    if (inflateInit2(&stream_, window_bits) != Z_OK) {
        allocation_.rethrow_allocation_failure();
    }
}

zlib_inflate_stream::~zlib_inflate_stream() {
    if (initialized()) {
        (void)inflateEnd(&stream_);
    }
}

bool zlib_inflate_stream::reset() noexcept {
    allocation_.clear_allocation_failure();
    return initialized() && inflateReset(&stream_) == Z_OK;
}

zlib_deflate_stream::zlib_deflate_stream(
    std::pmr::memory_resource* resource, int level, int window_bits, int mem_level, int strategy)
    : allocation_(resource),
      stream_(make_zlib_stream(allocation_)) {
    if (deflateInit2(&stream_, level, Z_DEFLATED, window_bits, mem_level, strategy) != Z_OK) {
        allocation_.rethrow_allocation_failure();
    }
}

zlib_deflate_stream::zlib_deflate_stream(std::pmr::memory_resource* resource, zlib_content_format format)
    : zlib_deflate_stream(resource, Z_DEFAULT_COMPRESSION,
          format == zlib_content_format::gzip ? zlib_gzip_window_bits : zlib_deflate_window_bits,
          zlib_default_mem_level, Z_DEFAULT_STRATEGY) {}

zlib_deflate_stream::~zlib_deflate_stream() {
    if (initialized()) {
        (void)deflateEnd(&stream_);
    }
}

bool zlib_deflate_stream::reset() noexcept {
    allocation_.clear_allocation_failure();
    return initialized() && deflateReset(&stream_) == Z_OK;
}

}  // namespace ruvia::detail
