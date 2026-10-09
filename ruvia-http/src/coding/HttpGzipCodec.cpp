#include <zlib.h>

#include <cstddef>
#include <limits>
#include <new>
#include <utility>

#include "ruvia/http/detail/util/PmrResource.h"

#include "coding/HttpContentCodec.h"
#include "coding/PmrCodecAllocation.h"
#include "coding/ZlibPmrAllocation.h"

// gzip (RFC 1952) and zlib-wrapped deflate through zlib, with zlib's allocator
// routed to the caller's memory resource so neither direction makes a global allocation.

namespace ruvia::detail {

namespace {

voidpf gzipZalloc(voidpf opaque, uInt items, uInt size) noexcept {
    return zlib_pmr_allocate_with_exception(opaque, items, size);
}

void gzipZfree(voidpf, voidpf address) noexcept {
    zlibPmrFree(address);
}

[[nodiscard]] z_stream makeGzipStream(void* allocation_context) noexcept {
    z_stream stream{};
    stream.zalloc = &gzipZalloc;
    stream.zfree = &gzipZfree;
    stream.opaque = allocation_context;
    return stream;
}

inline void refillGzipInput(
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

static HttpContentDecodeResult decode_zlib_content(std::string_view input,
    std::size_t maxDecodedBytes, std::pmr::memory_resource* resource, int windowBits,
    bool allowConcatenatedMembers) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    pmr_codec_allocation_context allocation_context(output.get_allocator().resource());
    auto stream = makeGzipStream(&allocation_context);
    if (inflateInit2(&stream, windowBits) != Z_OK) {
        allocation_context.rethrow_allocation_failure();
        return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kDecoderFailure);
    }
    struct Guard final {
        z_stream* stream;
        ~Guard() {
            (void)inflateEnd(stream);
        }
    } guard{&stream};

    std::size_t supplied = 0;
    char buffer[16384];
    for (;;) {
        refillGzipInput(stream, input, supplied);
        const auto beforeInput = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = static_cast<uInt>(sizeof(buffer));
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
            allocation_context.rethrow_allocation_failure();
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (!appendDecodedBytes(output, buffer, produced, maxDecodedBytes)) {
            return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kDecodedSizeExceeded);
        }

        if (status == Z_STREAM_END) {
            // RFC 1952 gzip data is a series of members. Preserve any input
            // already supplied to zlib, reset only the member state, and keep
            // decoding until the exact HTTP content boundary is consumed.
            refillGzipInput(stream, input, supplied);
            if (stream.avail_in == 0 && supplied == input.size()) {
                return HttpContentDecodeResultAccess::decoded(std::move(output));
            }
            if (!allowConcatenatedMembers) {
                return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kInvalidContent);
            }
            auto* nextInput = stream.next_in;
            const auto availableInput = stream.avail_in;
            const int reset = inflateReset2(&stream, windowBits);
            if (reset != Z_OK) {
                allocation_context.rethrow_allocation_failure();
                return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kDecoderFailure);
            }
            stream.next_in = nextInput;
            stream.avail_in = availableInput;
            continue;
        }
        if (status == Z_MEM_ERROR) {
            return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kDecoderFailure);
        }
        if (status != Z_OK && status != Z_BUF_ERROR) {
            return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kInvalidContent);
        }

        const bool progressed = produced != 0 || stream.avail_in != beforeInput;
        if (!progressed) {
            if (stream.avail_in == 0 && supplied < input.size()) {
                continue;
            }
            return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kInvalidContent);
        }
    }
}

static HttpContentEncodeResult encode_zlib_content(std::string_view input,
    std::size_t maxEncodedBytes, std::pmr::memory_resource* resource, int windowBits) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    pmr_codec_allocation_context allocation_context(output.get_allocator().resource());
    auto stream = makeGzipStream(&allocation_context);
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        allocation_context.rethrow_allocation_failure();
        return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncoderFailure);
    }
    struct Guard final {
        z_stream* stream;
        ~Guard() {
            (void)deflateEnd(stream);
        }
    } guard{&stream};

    std::size_t supplied = 0;
    for (;;) {
        refillGzipInput(stream, input, supplied);
        if (output.size() == maxEncodedBytes) {
            if (stream.avail_in == 0 && supplied == input.size()) {
                Bytef probe{};
                stream.next_out = &probe;
                stream.avail_out = 1;
                const auto status = deflate(&stream, Z_FINISH);
                if (status != Z_OK && status != Z_STREAM_END) {
                    allocation_context.rethrow_allocation_failure();
                }
                if (status == Z_STREAM_END && stream.avail_out == 1) {
                    return HttpContentEncodeResultAccess::encoded(std::move(output));
                }
                if (status == Z_MEM_ERROR) {
                    return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncoderFailure);
                }
            }
            return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncodedSizeExceeded);
        }
        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(8192, maxEncodedBytes - offset);
        const auto beforeInput = stream.avail_in;
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
            return HttpContentEncodeResultAccess::encoded(std::move(output));
        }
        if (status == Z_MEM_ERROR) {
            return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncoderFailure);
        }
        if (status != Z_OK || (output.size() == offset && stream.avail_in == beforeInput)) {
            return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncoderFailure);
        }
    }
}

HttpContentDecodeResult decodeGzipContent(
    std::string_view input, std::size_t maxDecodedBytes, std::pmr::memory_resource* resource) {
    return decode_zlib_content(input, maxDecodedBytes, resource, 15 + 16, true);
}

HttpContentDecodeResult decode_deflate_content(
    std::string_view input, std::size_t maxDecodedBytes, std::pmr::memory_resource* resource) {
    return decode_zlib_content(input, maxDecodedBytes, resource, 15, false);
}

HttpContentEncodeResult encodeGzipContent(
    std::string_view input, std::size_t maxEncodedBytes, std::pmr::memory_resource* resource) {
    return encode_zlib_content(input, maxEncodedBytes, resource, 15 + 16);
}

HttpContentEncodeResult encode_deflate_content(
    std::string_view input, std::size_t maxEncodedBytes, std::pmr::memory_resource* resource) {
    return encode_zlib_content(input, maxEncodedBytes, resource, 15);
}

}  // namespace ruvia::detail
