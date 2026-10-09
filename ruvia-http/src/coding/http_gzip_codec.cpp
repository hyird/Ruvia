#include <zlib.h>

#include <cstddef>
#include <limits>
#include <new>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/http_content_codec.h"
#include "coding/pmr_codec_allocation.h"
#include "coding/zlib_pmr_allocation.h"

// gzip (RFC 1952) and zlib-wrapped deflate through zlib, with zlib's allocator
// routed to the caller's memory resource so neither direction makes a global allocation.

namespace ruvia::detail {

namespace {

voidpf gzip_zalloc(voidpf opaque, uInt items, uInt size) noexcept {
    return zlib_pmr_allocate_with_exception(opaque, items, size);
}

void gzip_zfree(voidpf, voidpf address) noexcept {
    zlib_pmr_free(address);
}

[[nodiscard]] z_stream make_gzip_stream(void* allocation_context) noexcept {
    z_stream stream{};
    stream.zalloc = &gzip_zalloc;
    stream.zfree = &gzip_zfree;
    stream.opaque = allocation_context;
    return stream;
}

inline void refill_gzip_input(
    z_stream& stream, std::string_view input, std::size_t& supplied) noexcept {
    if (stream.avail_in != 0 || supplied == input.size()) {
        return;
    }
    const auto count = static_cast<uInt>(
        std::min<std::size_t>(input.size() - supplied, (std::numeric_limits<uInt>::max)()));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data() + supplied));
    stream.avail_in = count;
    supplied += count;
}

}  // namespace

static http_content_decode_result decode_zlib_content(std::string_view input,
    std::size_t max_decoded_bytes, std::pmr::memory_resource* resource, int window_bits,
    bool allow_concatenated_members) {
    std::pmr::string output(http_pmr_resource_or_default(resource));
    pmr_codec_allocation_context allocation_context(output.get_allocator().resource());
    auto stream = make_gzip_stream(&allocation_context);
    if (inflateInit2(&stream, window_bits) != Z_OK) {
        allocation_context.rethrow_allocation_failure();
        return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
    }
    struct guard final {
        z_stream* stream_;
        ~guard() {
            (void)inflateEnd(stream_);
        }
    } guard_value{&stream};

    std::size_t supplied = 0;
    char buffer[16384];
    for (;;) {
        refill_gzip_input(stream, input, supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = static_cast<uInt>(sizeof(buffer));
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
            allocation_context.rethrow_allocation_failure();
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (!append_decoded_bytes(output, buffer, produced, max_decoded_bytes)) {
            return http_content_decode_result_access::failure(http_content_decode_error::decoded_size_exceeded);
        }

        if (status == Z_STREAM_END) {
            // RFC 1952 gzip data is a series of members. Preserve any input
            // already supplied to zlib, reset only the member state, and keep
            // decoding until the exact HTTP content boundary is consumed.
            refill_gzip_input(stream, input, supplied);
            if (stream.avail_in == 0 && supplied == input.size()) {
                return http_content_decode_result_access::decoded(std::move(output));
            }
            if (!allow_concatenated_members) {
                return http_content_decode_result_access::failure(http_content_decode_error::invalid_content);
            }
            auto* next_input = stream.next_in;
            const auto available_input = stream.avail_in;
            const int reset = inflateReset2(&stream, window_bits);
            if (reset != Z_OK) {
                allocation_context.rethrow_allocation_failure();
                return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
            }
            stream.next_in = next_input;
            stream.avail_in = available_input;
            continue;
        }
        if (status == Z_MEM_ERROR) {
            return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
        }
        if (status != Z_OK && status != Z_BUF_ERROR) {
            return http_content_decode_result_access::failure(http_content_decode_error::invalid_content);
        }

        const bool progressed = produced != 0 || stream.avail_in != before_input;
        if (!progressed) {
            if (stream.avail_in == 0 && supplied < input.size()) {
                continue;
            }
            return http_content_decode_result_access::failure(http_content_decode_error::invalid_content);
        }
    }
}

static http_content_encode_result encode_zlib_content(std::string_view input,
    std::size_t max_encoded_bytes, std::pmr::memory_resource* resource, int window_bits) {
    std::pmr::string output(http_pmr_resource_or_default(resource));
    pmr_codec_allocation_context allocation_context(output.get_allocator().resource());
    auto stream = make_gzip_stream(&allocation_context);
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        allocation_context.rethrow_allocation_failure();
        return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
    }
    struct guard final {
        z_stream* stream_;
        ~guard() {
            (void)deflateEnd(stream_);
        }
    } guard_value{&stream};

    std::size_t supplied = 0;
    for (;;) {
        refill_gzip_input(stream, input, supplied);
        if (output.size() == max_encoded_bytes) {
            if (stream.avail_in == 0 && supplied == input.size()) {
                Bytef probe_value{};
                stream.next_out = &probe_value;
                stream.avail_out = 1;
                const auto status = deflate(&stream, Z_FINISH);
                if (status != Z_OK && status != Z_STREAM_END) {
                    allocation_context.rethrow_allocation_failure();
                }
                if (status == Z_STREAM_END && stream.avail_out == 1) {
                    return http_content_encode_result_access::encoded(std::move(output));
                }
                if (status == Z_MEM_ERROR) {
                    return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
                }
            }
            return http_content_encode_result_access::failure(http_content_encode_error::encoded_size_exceeded);
        }
        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(8192, max_encoded_bytes - offset);
        const auto before_input = stream.avail_in;
        int status = Z_OK;
        output.resize(offset + writable);
        stream.next_out = reinterpret_cast<Bytef*>(output.data() + offset);
        stream.avail_out = static_cast<uInt>(writable);
        status = deflate(&stream, stream.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH);
        output.resize(offset + (writable - stream.avail_out));
        if (status != Z_OK && status != Z_STREAM_END) {
            allocation_context.rethrow_allocation_failure();
        }
        if (status == Z_STREAM_END) {
            return http_content_encode_result_access::encoded(std::move(output));
        }
        if (status == Z_MEM_ERROR) {
            return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
        }
        if (status != Z_OK || (output.size() == offset && stream.avail_in == before_input)) {
            return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
        }
    }
}

http_content_decode_result decode_gzip_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    return decode_zlib_content(input, max_decoded_bytes, resource, 15 + 16, true);
}

http_content_decode_result decode_deflate_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    return decode_zlib_content(input, max_decoded_bytes, resource, 15, false);
}

http_content_encode_result encode_gzip_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource) {
    return encode_zlib_content(input, max_encoded_bytes, resource, 15 + 16);
}

http_content_encode_result encode_deflate_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource) {
    return encode_zlib_content(input, max_encoded_bytes, resource, 15);
}

}  // namespace ruvia::detail
