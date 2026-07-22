#include "ruvia/http/detail/coding/HttpContentCodec.h"

#include <cstddef>
#include <limits>
#include <new>
#include <utility>

#include <zlib.h>

#include "ruvia/http/detail/util/PmrResource.h"

// gzip (RFC 1952) through zlib, with zlib's allocator routed to the caller's
// memory resource so neither direction makes a global allocation.

namespace ruvia::detail {

namespace {

struct alignas(std::max_align_t) ZlibAllocationHeader final {
    std::pmr::memory_resource* resource;
    std::size_t bytes;
};

voidpf gzipZalloc(voidpf opaque, uInt items, uInt size) noexcept {
    auto* resource = static_cast<std::pmr::memory_resource*>(opaque);
    if (resource == nullptr || items == 0 || size == 0) {
        return nullptr;
    }
    const auto itemBytes = static_cast<std::size_t>(items);
    const auto sizeBytes = static_cast<std::size_t>(size);
    if (itemBytes > (std::numeric_limits<std::size_t>::max)() / sizeBytes) {
        return nullptr;
    }
    const auto payloadBytes = itemBytes * sizeBytes;
    if (payloadBytes > (std::numeric_limits<std::size_t>::max)() - sizeof(ZlibAllocationHeader)) {
        return nullptr;
    }
    const auto totalBytes = sizeof(ZlibAllocationHeader) + payloadBytes;
    try {
        auto* raw = static_cast<std::byte*>(resource->allocate(totalBytes, alignof(ZlibAllocationHeader)));
        auto* header = reinterpret_cast<ZlibAllocationHeader*>(raw);
        header->resource = resource;
        header->bytes = totalBytes;
        return raw + sizeof(ZlibAllocationHeader);
    } catch (...) {
        return nullptr;
    }
}

void gzipZfree(voidpf, voidpf address) noexcept {
    if (address == nullptr) {
        return;
    }
    auto* raw = static_cast<std::byte*>(address) - sizeof(ZlibAllocationHeader);
    auto* header = reinterpret_cast<ZlibAllocationHeader*>(raw);
    header->resource->deallocate(raw, header->bytes, alignof(ZlibAllocationHeader));
}

}  // namespace

ContentDecodeAttempt decodeGzipContent(
    std::string_view input,
    std::size_t maxDecodedBytes,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        return HttpContentDecodeError::kDecoderFailure;
    }
    struct Guard final {
        z_stream* stream;
        ~Guard() { (void)inflateEnd(stream); }
    } guard{&stream};

    std::size_t supplied = 0;
    const auto refill = [&]() noexcept {
        if (stream.avail_in != 0 || supplied == input.size()) {
            return;
        }
        const auto count = static_cast<uInt>(std::min<std::size_t>(
            input.size() - supplied,
            (std::numeric_limits<uInt>::max)()));
        stream.next_in = reinterpret_cast<Bytef*>(
            const_cast<char*>(input.data() + supplied));
        stream.avail_in = count;
        supplied += count;
    };

    char buffer[16384];
    for (;;) {
        refill();
        const auto beforeInput = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = static_cast<uInt>(sizeof(buffer));
        const int status = inflate(&stream, Z_NO_FLUSH);
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (!appendDecodedBytes(
                output,
                buffer,
                produced,
                maxDecodedBytes)) {
            return HttpContentDecodeError::kDecodedSizeExceeded;
        }

        if (status == Z_STREAM_END) {
            // RFC 1952 gzip data is a series of members. Preserve any input
            // already supplied to zlib, reset only the member state, and keep
            // decoding until the exact HTTP content boundary is consumed.
            refill();
            if (stream.avail_in == 0 && supplied == input.size()) {
                return output;
            }
            auto* nextInput = stream.next_in;
            const auto availableInput = stream.avail_in;
            const int reset = inflateReset2(&stream, 15 + 16);
            if (reset != Z_OK) {
                return HttpContentDecodeError::kDecoderFailure;
            }
            stream.next_in = nextInput;
            stream.avail_in = availableInput;
            continue;
        }
        if (status == Z_MEM_ERROR) {
            return HttpContentDecodeError::kDecoderFailure;
        }
        if (status != Z_OK && status != Z_BUF_ERROR) {
            return HttpContentDecodeError::kInvalidContent;
        }

        const bool progressed =
            produced != 0 || stream.avail_in != beforeInput;
        if (!progressed) {
            if (stream.avail_in == 0 && supplied < input.size()) {
                continue;
            }
            return HttpContentDecodeError::kInvalidContent;
        }
    }
}

ContentEncodeAttempt encodeGzipContent(
    std::string_view input,
    std::size_t maxEncodedBytes,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    z_stream stream{};
    stream.zalloc = &gzipZalloc;
    stream.zfree = &gzipZfree;
    stream.opaque = output.get_allocator().resource();
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return HttpContentEncodeError::kEncoderFailure;
    }
    struct Guard final {
        z_stream* stream;
        ~Guard() { (void)deflateEnd(stream); }
    } guard{&stream};

    std::size_t supplied = 0;
    const auto refill = [&]() noexcept {
        if (stream.avail_in != 0 || supplied == input.size()) {
            return;
        }
        const auto count = static_cast<uInt>(std::min<std::size_t>(
            input.size() - supplied,
            (std::numeric_limits<uInt>::max)()));
        stream.next_in = reinterpret_cast<Bytef*>(
            const_cast<char*>(input.data() + supplied));
        stream.avail_in = count;
        supplied += count;
    };

    for (;;) {
        refill();
        if (output.size() == maxEncodedBytes) {
            if (stream.avail_in == 0 && supplied == input.size()) {
                Bytef probe{};
                stream.next_out = &probe;
                stream.avail_out = 1;
                const auto status = deflate(&stream, Z_FINISH);
                if (status == Z_STREAM_END && stream.avail_out == 1) {
                    return output;
                }
                if (status == Z_MEM_ERROR) {
                    return HttpContentEncodeError::kEncoderFailure;
                }
            }
            return HttpContentEncodeError::kEncodedSizeExceeded;
        }
        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(
            8192,
            maxEncodedBytes - offset);
        const auto beforeInput = stream.avail_in;
        output.resize(offset + writable);
        stream.next_out = reinterpret_cast<Bytef*>(output.data() + offset);
        stream.avail_out = static_cast<uInt>(writable);
        const auto status = deflate(
            &stream,
            stream.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH);
        output.resize(offset + (writable - stream.avail_out));
        if (status == Z_STREAM_END) {
            return output;
        }
        if (status == Z_MEM_ERROR) {
            return HttpContentEncodeError::kEncoderFailure;
        }
        if (status != Z_OK ||
            (output.size() == offset && stream.avail_in == beforeInput)) {
            return HttpContentEncodeError::kEncoderFailure;
        }
    }
}

}  // namespace ruvia::detail
