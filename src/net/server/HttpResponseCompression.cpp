#include "HttpResponseCompression.h"

#include "../../http/HttpResponseBodyAccess.h"
#include "../../http/HttpResponseFileAccess.h"
#include "../../http/HeaderTokenUtils.h"
#include "../../http/ResponseHeaderUtils.h"
#include "ruvia/http/detail/PmrString.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string_view>

#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

namespace ruvia::detail {
namespace {

void setCompressedContentLength(HttpResponse& response, std::size_t size) {
    setResponseHeaderUnsigned(
        response,
        "Content-Length",
        static_cast<std::uint64_t>(size),
        kResponseHeaderContentLength);
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

// Quality 5 is the dynamic-compression sweet spot: noticeably faster than the
// archival default (11) while still beating gzip on ratio. Brotli and zstd both
// allocate their transient working memory through the global allocator, which
// this build routes to mimalloc, so no custom resource plumbing is needed.
constexpr int kBrotliQuality = 5;

bool brotliCompress(std::string_view input, std::pmr::string& output, std::size_t maxOutputBytes) {
    bool ok = false;
    output.resize_and_overwrite(
        maxOutputBytes,
        [&input, &ok](char* data, std::size_t count) noexcept {
            std::size_t encodedSize = count;
            ok = BrotliEncoderCompress(
                     kBrotliQuality,
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
            const std::size_t result =
                ZSTD_compress(data, count, input.data(), input.size(), ZSTD_CLEVEL_DEFAULT);
            ok = ZSTD_isError(result) == 0;
            return ok ? result : std::size_t{0};
        });
    return ok;
}

struct CodingCompressor final {
    bool (*compress)(std::string_view, std::pmr::string&, std::size_t){nullptr};
    std::string_view token;
};

[[nodiscard]] CodingCompressor codingCompressor(HttpContentCoding coding) noexcept {
    switch (coding) {
        case HttpContentCoding::kBrotli:
            return {&brotliCompress, "br"};
        case HttpContentCoding::kZstd:
            return {&zstdCompress, "zstd"};
        case HttpContentCoding::kGzip:
            return {&gzipCompress, "gzip"};
        case HttpContentCoding::kNone:
            break;
    }
    return {};
}

[[nodiscard]] bool mediaTypeStartsWith(std::string_view mediaType, std::string_view prefix) noexcept {
    return mediaType.size() >= prefix.size() &&
        asciiEqualsIgnoreCase(mediaType.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool responseContentTypeSkipsCompression(std::string_view contentType) noexcept {
    if (contentType.empty()) {
        return false;
    }
    const auto semicolon = contentType.find(';');
    const auto mediaType = httpTrimOws(
        semicolon == std::string_view::npos ? contentType : contentType.substr(0, semicolon));
    if (mediaType.empty()) {
        return false;
    }
    if (asciiEqualsIgnoreCase(mediaType, "image/svg+xml")) {
        return false;
    }
    return mediaTypeStartsWith(mediaType, "image/") ||
        mediaTypeStartsWith(mediaType, "video/") ||
        mediaTypeStartsWith(mediaType, "audio/") ||
        asciiEqualsIgnoreCase(mediaType, "application/gzip") ||
        asciiEqualsIgnoreCase(mediaType, "application/x-gzip") ||
        asciiEqualsIgnoreCase(mediaType, "application/zip") ||
        asciiEqualsIgnoreCase(mediaType, "application/zstd") ||
        asciiEqualsIgnoreCase(mediaType, "application/pdf") ||
        asciiEqualsIgnoreCase(mediaType, "application/octet-stream");
}

}  // namespace

bool compressResponseBodyIfAccepted(
    HttpContentCoding coding,
    HttpResponse& response,
    const HttpServerOptions::Compression& options,
    std::pmr::string& compressionScratch,
    bool skipBody) {
    if (skipBody ||
        !options.enabled ||
        coding == HttpContentCoding::kNone) {
        return false;
    }
    const auto compressor = codingCompressor(coding);

    const auto statusCode = response.status();
    if (statusCode < 200 ||
        statusCode == 206 ||
        statusCode == 204 ||
        statusCode == 205 ||
        statusCode == 304) {
        return false;
    }

    if (responseHasFileBody(response) ||
        responseBodySize(response) < options.minBytes ||
        responseHasKnownHeader(response, kResponseHeaderContentEncoding) ||
        responseHasKnownHeader(response, kResponseHeaderContentRange) ||
        responseContentTypeSkipsCompression(responseKnownHeader(response, kResponseHeaderContentType)) ||
        httpHasToken(responseKnownHeader(response, kResponseHeaderCacheControl), "no-transform")) {
        return false;
    }

    const auto body = responseBodyBytes(response);
    compressionScratch.clear();
    compressionScratch.reserve(body.size());
    if (compressor.compress == nullptr ||
        !compressor.compress(body, compressionScratch, body.size()) ||
        compressionScratch.size() >= body.size()) {
        clearPmrStringRetainingSmall(compressionScratch, kCompressionScratchRetainedBytes);
        return false;
    }

    setResponseHeaderStableView(response, "Content-Encoding", compressor.token);
    addVaryToken(response, "Accept-Encoding");
    setCompressedContentLength(response, compressionScratch.size());
    response.setBodyView(compressionScratch);
    return true;
}

}  // namespace ruvia::detail
