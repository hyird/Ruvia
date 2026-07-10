#include "ruvia/http/detail/HttpContentCoding.h"

#include <algorithm>
#include <cstdint>
#include <limits>

#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

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

bool gzipCompress(std::string_view input, std::pmr::string& output, std::size_t maxOutputBytes) {
    z_stream stream{};
    stream.zalloc = &gzipZalloc;
    stream.zfree = &gzipZfree;
    stream.opaque = output.get_allocator().resource();
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    struct Guard final {
        z_stream* stream;
        ~Guard() { (void)deflateEnd(stream); }
    } guard{&stream};

    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    int status = Z_OK;
    while (status == Z_OK) {
        if (output.size() >= maxOutputBytes) {
            return false;
        }
        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(8192, maxOutputBytes - offset);
        output.resize_and_overwrite(
            offset + writable,
            [&stream, &status, offset](char* data, std::size_t count) noexcept {
                const auto available = count - offset;
                stream.next_out = reinterpret_cast<Bytef*>(data + offset);
                stream.avail_out = static_cast<uInt>(available);
                status = deflate(&stream, Z_FINISH);
                return offset + (available - stream.avail_out);
            });
        if (status != Z_OK && status != Z_STREAM_END) {
            return false;
        }
    }
    return status == Z_STREAM_END;
}

bool brotliCompress(std::string_view input, std::pmr::string& output, std::size_t maxOutputBytes) {
    bool ok = false;
    output.resize_and_overwrite(
        maxOutputBytes,
        [&input, &ok](char* data, std::size_t count) noexcept {
            std::size_t encodedSize = count;
            ok = BrotliEncoderCompress(
                     5,
                     BROTLI_DEFAULT_WINDOW,
                     BROTLI_MODE_GENERIC,
                     input.size(),
                     reinterpret_cast<const std::uint8_t*>(input.data()),
                     &encodedSize,
                     reinterpret_cast<std::uint8_t*>(data)) == BROTLI_TRUE;
            return ok ? encodedSize : std::size_t{0};
        });
    return ok;
}

bool zstdCompress(std::string_view input, std::pmr::string& output, std::size_t maxOutputBytes) {
    bool ok = false;
    output.resize_and_overwrite(
        maxOutputBytes,
        [&input, &ok](char* data, std::size_t count) noexcept {
            const auto result = ZSTD_compress(data, count, input.data(), input.size(), ZSTD_CLEVEL_DEFAULT);
            ok = ZSTD_isError(result) == 0;
            return ok ? result : std::size_t{0};
        });
    return ok;
}

}  // namespace

std::string_view httpContentCodingToken(HttpContentCoding coding) noexcept {
    switch (coding) {
        case HttpContentCoding::kBrotli:
            return "br";
        case HttpContentCoding::kZstd:
            return "zstd";
        case HttpContentCoding::kGzip:
            return "gzip";
        case HttpContentCoding::kNone:
            return {};
    }
    return {};
}

bool encodeHttpContent(
    HttpContentCoding coding,
    std::string_view input,
    std::pmr::string& output,
    std::size_t maxOutputBytes) {
    switch (coding) {
        case HttpContentCoding::kBrotli:
            return brotliCompress(input, output, maxOutputBytes);
        case HttpContentCoding::kZstd:
            return zstdCompress(input, output, maxOutputBytes);
        case HttpContentCoding::kGzip:
            return gzipCompress(input, output, maxOutputBytes);
        case HttpContentCoding::kNone:
            return false;
    }
    return false;
}

}  // namespace ruvia::detail
