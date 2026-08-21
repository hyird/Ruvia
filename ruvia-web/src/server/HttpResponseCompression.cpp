#include "ruvia/web/detail/server/response/HttpResponseCompression.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"

#include "ruvia/http/HttpCache.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/detail/response/ResponseHeaderUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/core/memory/ProcessResource.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ruvia::detail {
namespace {

enum class BufferedCompressionAttemptStatus : std::uint8_t {
    kCompressed,
    kNotSmaller,
    kFailed,
};

struct BufferedCompressionAttempt final {
    explicit BufferedCompressionAttempt(BufferedCompressionAttemptStatus status) noexcept
        : status(status), bytes(processResource()) {}

    BufferedCompressionAttempt(BufferedCompressionAttemptStatus status, std::pmr::string bytes) noexcept
        : status(status), bytes(std::move(bytes)) {}

    BufferedCompressionAttemptStatus status;
    std::pmr::string bytes;
};

[[nodiscard]] BufferedCompressionAttempt encodeBufferedBody(HttpContentCoding coding, std::pmr::string plain) {
    const auto maxEncodedBytes = plain.empty() ? 0 : plain.size() - 1;
    auto encoding = encodeHttpContent(coding, plain, maxEncodedBytes, processResource());
    if (auto* encoded = encoding.encoded(); encoded != nullptr) {
        return BufferedCompressionAttempt(BufferedCompressionAttemptStatus::kCompressed, std::move(*encoded).takeBytes());
    }
    const auto* failure = encoding.failure();
    if (failure != nullptr && failure->error() == HttpContentEncodeError::kEncodedSizeExceeded) {
        return BufferedCompressionAttempt(BufferedCompressionAttemptStatus::kNotSmaller);
    }
    return BufferedCompressionAttempt(BufferedCompressionAttemptStatus::kFailed);
}

[[nodiscard]] bool mediaTypeStartsWith(std::string_view mediaType, std::string_view prefix) noexcept {
    return mediaType.size() >= prefix.size() && httpAsciiEqualsIgnoreCase(mediaType.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool responseContentTypeSkipsCompression(std::string_view contentType) noexcept {
    if (contentType.empty()) {
        return false;
    }
    const auto semicolon = contentType.find(';');
    const auto mediaType = httpTrimOws(semicolon == std::string_view::npos ? contentType : contentType.substr(0, semicolon));
    if (mediaType.empty()) {
        return false;
    }
    // Dominant compressible types short-circuit the skip list below.
    if (mediaTypeStartsWith(mediaType, "text/") || httpAsciiEqualsIgnoreCase(mediaType, "application/json")) {
        return false;
    }
    if (httpAsciiEqualsIgnoreCase(mediaType, "image/svg+xml")) {
        return false;
    }
    return mediaTypeStartsWith(mediaType, "image/") || mediaTypeStartsWith(mediaType, "video/") || mediaTypeStartsWith(mediaType, "audio/") || httpAsciiEqualsIgnoreCase(mediaType, "application/gzip") || httpAsciiEqualsIgnoreCase(mediaType, "application/x-gzip") || httpAsciiEqualsIgnoreCase(mediaType, "application/zip") || httpAsciiEqualsIgnoreCase(mediaType, "application/zstd") || httpAsciiEqualsIgnoreCase(mediaType, "application/pdf") || httpAsciiEqualsIgnoreCase(mediaType, "application/octet-stream");
}

[[nodiscard]] CacheControl responseCacheControl(const HttpResponse& response) noexcept {
    CacheControlFieldParser parser;
    if (!responseHasKnownHeader(response, kResponseHeaderCacheControl)) {
        return parser.finish();
    }
    for (const auto& header : response.headers()) {
        if (responseHeaderKnownBit(header) == kResponseHeaderCacheControl) {
            parser.update(header.value());
        }
    }
    return parser.finish();
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

HttpResponseCompressionEligibility httpResponseCompressionEligibility(const HttpResponseCodingSelection& /*selection*/, HttpKnownMethod requestMethod, const HttpResponse& response, ResponseStreamKind kind) noexcept {
    const auto bodyPlan = httpResponseBodyPlan(requestMethod, response.status());
    if (!bodyPlan.statusAllowsBody()) {
        return HttpResponseCompressionEligibility::kIneligible;
    }

    const auto statusCode = response.status();
    if (statusCode == http_status::kPartialContent || statusCode == http_status::kResetContent) {
        return HttpResponseCompressionEligibility::kIneligible;
    }

    if (responseHasKnownHeader(response, kResponseHeaderContentEncoding) || responseHasKnownHeader(response, kResponseHeaderContentRange) || (kind != ResponseStreamKind::kSse && responseContentTypeSkipsCompression(responseKnownHeader(response, kResponseHeaderContentType))) || responseCacheControl(response).noTransform) {
        return HttpResponseCompressionEligibility::kIneligible;
    }
    return HttpResponseCompressionEligibility::kEligible;
}

HttpResponseCompressionResult applyResponseCompression(const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod, HttpResponse& response, const CompressionConfig& options) {
    const auto& responseContent = responseBody(response);

    // These responses never vary by Accept-Encoding, so they are served identity
    // with no Vary (RFC 9110 12.5.5 SHOULD NOT list a field that does not affect
    // the representation): a file body (framed and Vary'd by the static-file path),
    // an already-chosen Content-Encoding, a Content-Range, an incompressible media
    // type, or an explicit no-transform.
    if (responseContent.file().has_value() || httpResponseCompressionEligibility(selection, requestMethod, response, ResponseStreamKind::kGeneric) != HttpResponseCompressionEligibility::kEligible) {
        return HttpResponseCompressionResult::makeNotApplicable();
    }

    const auto coding = selection.coding();

    // A compressible representation IS selected by Accept-Encoding, so it varies by
    // it even when this particular response is left identity -- because the client
    // accepted no coding we support, or the body is below the size threshold. Set
    // Vary regardless of the outcome so a shared cache never serves this identity
    // body to a client that would receive the compressed one (RFC 9110 12.5.5); it
    // previously lived only on the compress-success path.
    addVaryToken(response, "Accept-Encoding");

    if (coding == HttpContentCoding::kIdentity || responseContent.size() < options.minBytes || responseContent.size() > options.maxBytes || responseContent.size() > options.syncBytes) {
        return HttpResponseCompressionResult::makeNotApplicable();
    }
    const auto body = responseContent.bytes();
    const auto maxEncodedBytes = body.empty() ? 0 : body.size() - 1;
    std::optional<HttpContentEncodeResult> encoding;
    try {
        encoding.emplace(encodeHttpContent(coding, body, maxEncodedBytes, responseResource(response)));
    } catch (...) {
        return HttpResponseCompressionResult::makeFailed();
    }
    auto* encoded = encoding->encoded();
    if (encoded == nullptr) {
        const auto* failure = encoding->failure();
        if (failure != nullptr && failure->error() == HttpContentEncodeError::kEncodedSizeExceeded) {
            return HttpResponseCompressionResult::makeNotApplicable();
        }
        return HttpResponseCompressionResult::makeFailed();
    }

    try {
        replaceResponseBodyWithContentEncoding(response, std::move(*encoded).takeBytes(), httpContentCodingToken(coding));
    } catch (...) {
        // The representation commit stages every affected header before
        // publishing the owned body. A request-resource failure therefore
        // leaves the identity response usable for the typed 500 path instead
        // of exposing a mixed body/metadata state.
        return HttpResponseCompressionResult::makeFailed();
    }
    return HttpResponseCompressionResult::makeCompressed();
}

Task<HttpResponseCompressionResult> applyResponseCompressionAsync(const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod, HttpResponse& response, CompressionConfig options, BlockingPool* pool, const WorkerHandle& worker) {
    const auto& responseContent = responseBody(response);
    if (responseContent.file().has_value() || httpResponseCompressionEligibility(selection, requestMethod, response, ResponseStreamKind::kGeneric) != HttpResponseCompressionEligibility::kEligible) {
        co_return HttpResponseCompressionResult::makeNotApplicable();
    }

    const auto coding = selection.coding();
    const auto size = responseContent.size();
    if (size <= options.syncBytes) {
        co_return applyResponseCompression(selection, requestMethod, response, options);
    }
    if (pool == nullptr) {
        // An explicitly disabled pool removes the offload boundary, not the
        // configured compression policy. Extend the synchronous range through
        // maxBytes and keep the same eligibility/commit behavior.
        options.syncBytes = options.maxBytes;
        co_return applyResponseCompression(selection, requestMethod, response, options);
    }
    addVaryToken(response, "Accept-Encoding");
    if (coding == HttpContentCoding::kIdentity || size < options.minBytes || size > options.maxBytes) {
        co_return HttpResponseCompressionResult::makeNotApplicable();
    }

    try {
        std::pmr::string plain(responseContent.bytes(), processResource());
        auto result = co_await tryRunBlocking(*pool, worker, [coding, plain = std::move(plain)]() mutable {
            return encodeBufferedBody(coding, std::move(plain));
        });
        if (result.failed()) {
            co_return HttpResponseCompressionResult::makeFailed();
        }
        if (!result.completed()) {
            // Queue saturation and shutdown are overload/lifecycle outcomes,
            // not broken encoders. Preserve the identity representation.
            co_return HttpResponseCompressionResult::makeNotApplicable();
        }
        auto attempt = std::move(result).value();
        if (attempt.status == BufferedCompressionAttemptStatus::kNotSmaller) {
            co_return HttpResponseCompressionResult::makeNotApplicable();
        }
        if (attempt.status != BufferedCompressionAttemptStatus::kCompressed || attempt.bytes.empty()) {
            co_return HttpResponseCompressionResult::makeFailed();
        }
        try {
            replaceResponseBodyWithContentEncoding(response, std::move(attempt.bytes), httpContentCodingToken(coding));
        } catch (...) {
            co_return HttpResponseCompressionResult::makeFailed();
        }
        co_return HttpResponseCompressionResult::makeCompressed();
    } catch (...) {
        // Failure to allocate/copy the request-owned input or create the
        // one-shot transport leaves the original identity body intact.
        co_return HttpResponseCompressionResult::makeFailed();
    }
}

bool prepareStreamingResponseCompression(const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod, HttpResponse& response, ResponseStreamKind kind) {
    if (selection.coding() == HttpContentCoding::kIdentity || httpResponseCompressionEligibility(selection, requestMethod, response, kind) != HttpResponseCompressionEligibility::kEligible) {
        return false;
    }

    addVaryToken(response, "Accept-Encoding");

    // The encoded length is not known until finish(), so a handler-provided
    // identity Content-Length cannot survive selecting a coding.
    response.removeHeader("Content-Length");
    setResponseHeaderStableView(response, "Content-Encoding", httpContentCodingToken(selection.coding()));
    weakenStrongResponseEtag(response);
    return true;
}

}  // namespace ruvia::detail
