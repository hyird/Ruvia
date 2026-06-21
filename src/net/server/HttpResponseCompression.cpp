#include "HttpResponseCompression.h"

#include "../../http/ResponseHeaderUtils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string_view>
#include <utility>

#include <zlib.h>

namespace ruvia::detail {
namespace {

void setCompressedContentLength(HttpResponse& response, std::size_t size) {
    setResponseHeaderUnsigned(
        response,
        "Content-Length",
        static_cast<std::uint64_t>(size),
        HttpResponse::kKnownHeaderContentLength);
}

struct alignas(std::max_align_t) ZlibAllocationHeader {
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
    auto* resource = output.get_allocator().resource();
    stream.zalloc = &gzipZalloc;
    stream.zfree = &gzipZfree;
    stream.opaque = resource;
    constexpr int kGzipWindowBits = 15 + 16;
    if (deflateInit2(
            &stream,
            Z_DEFAULT_COMPRESSION,
            Z_DEFLATED,
            kGzipWindowBits,
            8,
            Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }

    struct DeflateGuard final {
        z_stream* stream;
        ~DeflateGuard() { (void)deflateEnd(stream); }
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

}  // namespace

bool compressResponseBodyIfAccepted(
    const HttpRequestFlags& requestFlags,
    HttpResponse& response,
    const HttpServerOptions::Compression& options,
    std::pmr::string* compressionScratch,
    bool skipBody) {
    if (skipBody ||
        !options.enabled ||
        response.hasFileBody() ||
        response.bodySize() < options.minBytes ||
        response.hasKnownHeader(HttpResponse::kKnownHeaderContentEncoding) ||
        response.hasKnownHeader(HttpResponse::kKnownHeaderContentRange) ||
        httpHasToken(response.header(HttpResponse::kKnownHeaderCacheControl), "no-transform") ||
        !requestFlags.acceptsGzip) {
        return false;
    }
    if (response.statusCode() < 200 ||
        response.statusCode() == 206 ||
        response.statusCode() == 204 ||
        response.statusCode() == 205 ||
        response.statusCode() == 304) {
        return false;
    }

    const auto body = response.bodyBytes();
    std::pmr::string localCompressed(response.resource());
    auto& compressed = compressionScratch == nullptr ? localCompressed : *compressionScratch;
    compressed.clear();
    compressed.reserve(body.size());
    if (!gzipCompress(body, compressed, body.size()) || compressed.size() >= body.size()) {
        compressed.clear();
        return false;
    }

    setResponseHeaderStableView(response, "Content-Encoding", "gzip");
    addVaryToken(response, "Accept-Encoding");
    setCompressedContentLength(response, compressed.size());
    if (compressionScratch == nullptr) {
        response.setBody(std::move(compressed));
    } else {
        response.setBodyView(compressed);
    }
    return true;
}

}  // namespace ruvia::detail
