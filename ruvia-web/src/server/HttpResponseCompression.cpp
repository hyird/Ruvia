#include "ruvia/web/detail/server/HttpResponseCompression.h"

#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseFileAccess.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpContentCoding.h"
#include "ruvia/http/detail/ResponseHeaderUtils.h"
#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/http/detail/PmrString.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace ruvia::detail {
namespace {

void setCompressedContentLength(HttpResponse& response, std::size_t size) {
    setResponseHeaderUnsigned(
        response,
        "Content-Length",
        static_cast<std::uint64_t>(size),
        kResponseHeaderContentLength);
}

[[nodiscard]] bool mediaTypeStartsWith(std::string_view mediaType, std::string_view prefix) noexcept {
    return mediaType.size() >= prefix.size() &&
        httpAsciiEqualsIgnoreCase(mediaType.substr(0, prefix.size()), prefix);
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
    if (httpAsciiEqualsIgnoreCase(mediaType, "image/svg+xml")) {
        return false;
    }
    return mediaTypeStartsWith(mediaType, "image/") ||
        mediaTypeStartsWith(mediaType, "video/") ||
        mediaTypeStartsWith(mediaType, "audio/") ||
        httpAsciiEqualsIgnoreCase(mediaType, "application/gzip") ||
        httpAsciiEqualsIgnoreCase(mediaType, "application/x-gzip") ||
        httpAsciiEqualsIgnoreCase(mediaType, "application/zip") ||
        httpAsciiEqualsIgnoreCase(mediaType, "application/zstd") ||
        httpAsciiEqualsIgnoreCase(mediaType, "application/pdf") ||
        httpAsciiEqualsIgnoreCase(mediaType, "application/octet-stream");
}

// The handler's ETag validates its (identity) representation. Once the body is
// replaced with a content-coding, that is a different representation -- RFC 9110
// 8.8.1: "A strong validator ... changes ... whenever a change occurs to the
// representation data", and Content-Encoding is part of the representation. So a
// STRONG ETag must not remain attached byte-for-byte to the compressed body:
// otherwise a client holding the identity validator could issue a ranged
// If-Range and have the server splice compressed bytes into an identity copy, or
// a shared cache could treat the two encodings as interchangeable under strong
// comparison. Weaken it to a "W/"-prefixed weak validator -- the gzip and
// identity bodies are semantically equivalent, so If-None-Match revalidation
// still works, but strong (byte-exact) comparison is now forbidden. A tag that
// is already weak ("W/..."), malformed, or absent is left untouched.
void weakenStrongResponseEtag(HttpResponse& response) {
    if (!responseHasKnownHeader(response, kResponseHeaderEtag)) {
        return;
    }
    const auto etag = responseKnownHeader(response, kResponseHeaderEtag);
    if (etag.empty() || etag.front() != '"') {
        return;
    }
    std::pmr::string weak(responseResource(response));
    weak.reserve(etag.size() + 2);
    weak.append("W/");
    weak.append(etag.data(), etag.size());
    setResponseHeaderValidated(response, "ETag", weak, kResponseHeaderEtag);
}

}  // namespace

bool compressResponseBodyIfAccepted(
    HttpContentCoding coding,
    HttpResponse& response,
    const HttpServerOptions::Compression& options,
    std::pmr::string& compressionScratch,
    const HttpResponseBodyPlan& bodyPlan) {
    if (!bodyPlan.statusAllowsBody() || !options.enabled) {
        return false;
    }

    const auto statusCode = response.status();
    if (statusCode == 206 || statusCode == 205) {
        return false;
    }

    // These responses never vary by Accept-Encoding, so they are served identity
    // with no Vary (RFC 9110 12.5.5 SHOULD NOT list a field that does not affect
    // the representation): a file body (framed and Vary'd by the static-file path),
    // an already-chosen Content-Encoding, a Content-Range, an incompressible media
    // type, or an explicit no-transform.
    if (responseHasFileBody(response) ||
        responseHasKnownHeader(response, kResponseHeaderContentEncoding) ||
        responseHasKnownHeader(response, kResponseHeaderContentRange) ||
        responseContentTypeSkipsCompression(responseKnownHeader(response, kResponseHeaderContentType)) ||
        httpHasToken(responseKnownHeader(response, kResponseHeaderCacheControl), "no-transform")) {
        return false;
    }

    // A compressible representation IS selected by Accept-Encoding, so it varies by
    // it even when this particular response is left identity -- because the client
    // accepted no coding we support, or the body is below the size threshold. Set
    // Vary regardless of the outcome so a shared cache never serves this identity
    // body to a client that would receive the compressed one (RFC 9110 12.5.5); it
    // previously lived only on the compress-success path.
    addVaryToken(response, "Accept-Encoding");

    if (coding == HttpContentCoding::kNone ||
        responseBodySize(response) < options.minBytes) {
        return false;
    }
    const auto body = responseBodyBytes(response);
    compressionScratch.clear();
    compressionScratch.reserve(body.size());
    if (!encodeHttpContent(coding, body, compressionScratch, body.size()) ||
        compressionScratch.size() >= body.size()) {
        clearPmrStringRetainingSmall(compressionScratch, kCompressionScratchRetainedBytes);
        return false;
    }

    setResponseHeaderStableView(response, "Content-Encoding", httpContentCodingToken(coding));
    weakenStrongResponseEtag(response);
    setCompressedContentLength(response, compressionScratch.size());
    response.setBodyView(compressionScratch);
    return true;
}

}  // namespace ruvia::detail
