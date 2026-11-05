#include "coding/http_zlib_stream.h"

#include <array>
#include <cstddef>
#include <limits>

#include "ruvia/http/http_content_codec.h"

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

bool zlib_deflate_stream::encode(std::string_view input, std::pmr::string& output, int flush,
    std::size_t max_encoded_bytes) {
    if (output.size() > max_encoded_bytes) {
        return false;
    }
    const zlib_borrow_scope borrow(stream_);
    std::size_t supplied = 0;
    // Uninitialized scratch: only the bytes zlib produced are copied out.
    std::array<char, 16384> buffer;
    for (;;) {
        zlib_refill_input(stream_, input, supplied);
        // The requested flush applies once the last input window is handed
        // over: after Z_FINISH zlib accepts no further input.
        const bool last_window = supplied == input.size();
        const int operation = last_window ? flush : Z_NO_FLUSH;
        // At the cap, a one-byte probe distinguishes "already complete" from
        // "more output pending" without letting a byte past the cap through.
        const auto room = max_encoded_bytes - output.size();
        const bool probing = room == 0;
        const auto writable = probing ? std::size_t{1} : (std::min)(buffer.size(), room);
        const auto before_input = stream_.avail_in;
        stream_.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream_.avail_out = static_cast<uInt>(writable);
        const int status = deflate(&stream_, operation);
        const bool input_consumed = last_window && stream_.avail_in == 0;
        // Repeating a completed flush has no work left; zlib reports
        // Z_BUF_ERROR without invalidating the stream.
        if (status == Z_BUF_ERROR && operation != Z_FINISH && input_consumed) {
            return true;
        }
        if (status != Z_OK && status != Z_STREAM_END) {
            allocation_.rethrow_allocation_failure();
            throw http_content_codec_error("zlib content encoding failed");
        }
        const auto produced = writable - stream_.avail_out;
        if (probing && produced != 0) {
            return false;
        }
        output.append(buffer.data(), produced);
        if (status == Z_STREAM_END ||
            (operation != Z_FINISH && input_consumed && stream_.avail_out != 0)) {
            return true;
        }
        if (probing) {
            return false;
        }
        if (produced == 0 && stream_.avail_in == before_input) {
            throw http_content_codec_error("zlib content encoder made no progress");
        }
    }
}

}  // namespace ruvia::detail
