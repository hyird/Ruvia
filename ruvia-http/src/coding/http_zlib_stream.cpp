#include "coding/http_zlib_stream.h"

#include <zlib.h>

#include <array>
#include <cstddef>
#include <limits>
#include <utility>

#include "ruvia/http/http_content_codec.h"

#include "coding/http_content_codec.h"

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
    return http_c_library_memory::allocate(item_bytes * size_bytes, opaque);
}

[[nodiscard]] z_stream make_zlib_stream(http_c_library_memory& allocation) noexcept {
    z_stream stream{};
    stream.zalloc = &zlib_allocate;
    stream.zfree = &http_c_library_memory::deallocate_opaque_first;
    stream.opaque = &allocation;
    return stream;
}

}  // namespace

zlib_inflate_stream::zlib_inflate_stream(int window_bits, std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      stream_(make_zlib_stream(allocation_)) {
    if (inflateInit2(&stream_, window_bits) != Z_OK) {
        allocation_.rethrow_if_failed();
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
    int level, int window_bits, int mem_level, int strategy, std::size_t max_memory_bytes)
    : allocation_(max_memory_bytes, max_memory_bytes != (std::numeric_limits<std::size_t>::max)()),
      stream_(make_zlib_stream(allocation_)) {
    if (deflateInit2(&stream_, level, Z_DEFLATED, window_bits, mem_level, strategy) != Z_OK) {
        allocation_.rethrow_if_failed();
    }
}

zlib_deflate_stream::zlib_deflate_stream(zlib_content_format format, std::size_t max_memory_bytes)
    : zlib_deflate_stream(Z_DEFAULT_COMPRESSION,
          format == zlib_content_format::gzip ? zlib_gzip_window_bits : zlib_deflate_window_bits,
          zlib_default_mem_level, Z_DEFAULT_STRATEGY, max_memory_bytes) {}

zlib_deflate_stream::~zlib_deflate_stream() {
    if (initialized()) {
        (void)deflateEnd(&stream_);
    }
}

bool zlib_deflate_stream::reset() noexcept {
    allocation_.clear_allocation_failure();
    return initialized() && deflateReset(&stream_) == Z_OK;
}

bool zlib_deflate_stream::encode(std::string_view input, std::string& output, int flush,
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
            allocation_.rethrow_if_failed();
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

// gzip (RFC 1952) and zlib-wrapped deflate through zlib, with tracked working memory.

namespace ruvia::detail {

static std::variant<std::string, http_content_decode_error> decode_zlib_content(std::string_view input,
    std::size_t max_decoded_bytes, int window_bits,
    bool allow_concatenated_members) {
    std::string output;
    zlib_inflate_stream inflater(window_bits);
    if (!inflater.initialized()) {
        throw http_content_codec_error("zlib decoder initialization failed");
    }
    auto& stream = inflater.native();

    std::size_t supplied = 0;
    char buffer[16384];
    for (;;) {
        zlib_refill_input(stream, input, supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = static_cast<uInt>(sizeof(buffer));
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
            inflater.rethrow_allocation_failure();
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (!append_decoded_bytes(output, buffer, produced, max_decoded_bytes)) {
            return http_content_decode_error::decoded_size_exceeded;
        }

        if (status == Z_STREAM_END) {
            // RFC 1952 gzip data is a series of members. Preserve any input
            // already supplied to zlib, reset only the member state, and keep
            // decoding until the exact HTTP content boundary is consumed.
            zlib_refill_input(stream, input, supplied);
            if (stream.avail_in == 0 && supplied == input.size()) {
                return output;
            }
            if (!allow_concatenated_members) {
                return http_content_decode_error::invalid_content;
            }
            auto* next_input = stream.next_in;
            const auto available_input = stream.avail_in;
            const int reset = inflateReset2(&stream, window_bits);
            if (reset != Z_OK) {
                throw http_content_codec_error("failed to reset zlib content decoder");
            }
            stream.next_in = next_input;
            stream.avail_in = available_input;
            continue;
        }
        if (status == Z_MEM_ERROR) {
            throw http_content_codec_error("zlib decoder ran out of memory");
        }
        if (status != Z_OK && status != Z_BUF_ERROR) {
            return http_content_decode_error::invalid_content;
        }

        const bool progressed = produced != 0 || stream.avail_in != before_input;
        if (!progressed) {
            if (stream.avail_in == 0 && supplied < input.size()) {
                continue;
            }
            return http_content_decode_error::invalid_content;
        }
    }
}

std::optional<std::string> encode_zlib_content(zlib_deflate_stream& deflater,
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content_with_stream(deflater, Z_FINISH, input, max_encoded_bytes);
}
std::variant<std::string, http_content_decode_error> decode_gzip_content(
    std::string_view input, std::size_t max_decoded_bytes) {
    return decode_zlib_content(input, max_decoded_bytes, zlib_gzip_window_bits, true);
}

std::variant<std::string, http_content_decode_error> decode_deflate_content(
    std::string_view input, std::size_t max_decoded_bytes) {
    return decode_zlib_content(input, max_decoded_bytes, zlib_deflate_window_bits, false);
}

std::optional<std::string> encode_gzip_content(
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content<zlib_deflate_stream>(
        "zlib encoder initialization failed", Z_FINISH, input, max_encoded_bytes, zlib_content_format::gzip);
}

std::optional<std::string> encode_deflate_content(
    std::string_view input, std::size_t max_encoded_bytes) {
    return encode_whole_content<zlib_deflate_stream>("zlib encoder initialization failed", Z_FINISH, input,
        max_encoded_bytes, zlib_content_format::deflate);
}

}  // namespace ruvia::detail
