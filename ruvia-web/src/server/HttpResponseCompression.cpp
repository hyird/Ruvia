#include "ruvia/web/detail/server/response/HttpResponseCompression.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/HttpMediaType.h"

namespace ruvia::detail {
namespace {

enum class BufferedCompressionAttemptStatus : std::uint8_t {
    kCompressed,
    kNotSmaller,
    kFailed,
};

struct BufferedCompressionAttempt final {
    explicit BufferedCompressionAttempt(BufferedCompressionAttemptStatus status) noexcept
        : status(status),
          bytes(processResource()) {}

    BufferedCompressionAttempt(
        BufferedCompressionAttemptStatus status, std::pmr::string bytes) noexcept
        : status(status),
          bytes(std::move(bytes)) {}

    BufferedCompressionAttemptStatus status;
    std::pmr::string bytes;
};

[[nodiscard]] BufferedCompressionAttempt encodeBufferedBody(
    HttpContentCoding coding, std::pmr::string plain) {
    const auto maxEncodedBytes = plain.empty() ? 0 : plain.size() - 1;
    auto encoding = encodeHttpContent(
        coding, plain, {.maxEncodedBytes = maxEncodedBytes, .resource = processResource()});
    if (auto* encoded = encoding.encoded(); encoded != nullptr) {
        return BufferedCompressionAttempt(
            BufferedCompressionAttemptStatus::kCompressed, std::move(*encoded).takeBytes());
    }
    const auto* failure = encoding.failure();
    if (failure != nullptr && failure->error() == HttpContentEncodeError::kEncodedSizeExceeded) {
        return BufferedCompressionAttempt(BufferedCompressionAttemptStatus::kNotSmaller);
    }
    return BufferedCompressionAttempt(BufferedCompressionAttemptStatus::kFailed);
}

[[nodiscard]] bool mediaTypeStartsWith(
    std::string_view mediaType, std::string_view prefix) noexcept {
    return mediaType.size() >= prefix.size() &&
           httpAsciiEqualsIgnoreCase(mediaType.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool responseContentTypeSkipsCompression(std::string_view contentType) noexcept {
    if (contentType.empty()) {
        return false;
    }
    const auto mediaType = ::ruvia::httpMediaTypeOnly(contentType);
    if (mediaType.empty()) {
        return false;
    }
    // Dominant compressible types short-circuit the skip list below.
    if (mediaTypeStartsWith(mediaType, "text/") ||
        httpAsciiEqualsIgnoreCase(mediaType, "application/json")) {
        return false;
    }
    if (httpAsciiEqualsIgnoreCase(mediaType, "image/svg+xml")) {
        return false;
    }
    return mediaTypeStartsWith(mediaType, "image/") || mediaTypeStartsWith(mediaType, "video/") ||
           mediaTypeStartsWith(mediaType, "audio/") ||
           httpAsciiEqualsIgnoreCase(mediaType, "application/gzip") ||
           httpAsciiEqualsIgnoreCase(mediaType, "application/x-gzip") ||
           httpAsciiEqualsIgnoreCase(mediaType, "application/zip") ||
           httpAsciiEqualsIgnoreCase(mediaType, "application/zstd") ||
           httpAsciiEqualsIgnoreCase(mediaType, "application/pdf") ||
           httpAsciiEqualsIgnoreCase(mediaType, "application/octet-stream");
}

[[nodiscard]] CacheControl responseCacheControl(const HttpResponse& response) noexcept {
    CacheControlFieldParser parser;
    for (const auto& header : response.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Cache-Control")) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

}  // namespace

HttpResponseCompressionEligibility httpResponseCompressionEligibility(
    const HttpResponseCodingSelection& /*selection*/, HttpKnownMethod requestMethod,
    const HttpResponse& response, ResponseStreamKind kind) noexcept {
    const auto bodyPlan = planHttpResponseBody(requestMethod, response.status());
    if (!bodyPlan.statusAllowsBody()) {
        return HttpResponseCompressionEligibility::kIneligible;
    }

    const auto statusCode = response.status();
    if (statusCode == http_status::kPartialContent || statusCode == http_status::kResetContent) {
        return HttpResponseCompressionEligibility::kIneligible;
    }

    if (responseHasHeaderName(response, "Content-Encoding") ||
        responseHasHeaderName(response, "Content-Range") ||
        (kind != ResponseStreamKind::kSse &&
            responseContentTypeSkipsCompression(
                response.header("Content-Type").value_or(std::string_view{}))) ||
        responseCacheControl(response).has(CacheControlDirective::kNoTransform)) {
        return HttpResponseCompressionEligibility::kIneligible;
    }
    return HttpResponseCompressionEligibility::kEligible;
}

HttpResponseCompressionResult applyResponseCompression(const HttpResponseCodingSelection& selection,
    HttpKnownMethod requestMethod, HttpResponse& response, const CompressionConfig& options) {
    const auto responseContent = response.bodyBytes();

    // These responses never vary by Accept-Encoding, so they are served identity
    // with no Vary (RFC 9110 12.5.5 SHOULD NOT list a field that does not affect
    // the representation): a file body (framed and Vary'd by the static-file path),
    // an already-chosen Content-Encoding, a Content-Range, an incompressible media
    // type, or an explicit no-transform.
    if (response.fileBody().has_value() ||
        httpResponseCompressionEligibility(selection, requestMethod, response,
            ResponseStreamKind::kGeneric) != HttpResponseCompressionEligibility::kEligible) {
        return HttpResponseCompressionResult::makeNotApplicable();
    }

    const auto coding = selection.coding();

    // A compressible representation IS selected by Accept-Encoding, so it varies by
    // it even when this particular response is left identity -- because the client
    // accepted no coding we support, or the body is below the size threshold. Set
    // Vary regardless of the outcome so a shared cache never serves this identity
    // body to a client that would receive the compressed one (RFC 9110 12.5.5).
    response.addVaryToken("Accept-Encoding");

    if (coding == HttpContentCoding::kIdentity || responseContent.size() < options.minBytes ||
        responseContent.size() > options.maxBytes || responseContent.size() > options.syncBytes) {
        return HttpResponseCompressionResult::makeNotApplicable();
    }
    const auto body = responseContent;
    const auto maxEncodedBytes = body.empty() ? 0 : body.size() - 1;
    std::optional<HttpContentEncodeResult> encoding;
    try {
        encoding.emplace(encodeHttpContent(coding, body,
            {.maxEncodedBytes = maxEncodedBytes, .resource = response.memoryResource()}));
    } catch (...) {
        return HttpResponseCompressionResult::makeFailed();
    }
    auto* encoded = encoding->encoded();
    if (encoded == nullptr) {
        const auto* failure = encoding->failure();
        if (failure != nullptr &&
            failure->error() == HttpContentEncodeError::kEncodedSizeExceeded) {
            return HttpResponseCompressionResult::makeNotApplicable();
        }
        return HttpResponseCompressionResult::makeFailed();
    }

    try {
        response.replaceBodyWithContentEncoding(
            std::move(*encoded).takeBytes(), httpContentCodingToken(coding));
    } catch (...) {
        // The representation commit stages every affected header before
        // publishing the owned body. A request-resource failure therefore
        // leaves the identity response usable for the typed 500 path instead
        // of exposing a mixed body/metadata state.
        return HttpResponseCompressionResult::makeFailed();
    }
    return HttpResponseCompressionResult::makeCompressed();
}

Task<HttpResponseCompressionResult> applyResponseCompressionAsync(
    const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod,
    HttpResponse& response, CompressionConfig options, BlockingPool* pool,
    const WorkerHandle& worker) {
    const auto responseContent = response.bodyBytes();
    if (response.fileBody().has_value() ||
        httpResponseCompressionEligibility(selection, requestMethod, response,
            ResponseStreamKind::kGeneric) != HttpResponseCompressionEligibility::kEligible) {
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
    response.addVaryToken("Accept-Encoding");
    if (coding == HttpContentCoding::kIdentity || size < options.minBytes ||
        size > options.maxBytes) {
        co_return HttpResponseCompressionResult::makeNotApplicable();
    }

    try {
        std::pmr::string plain(responseContent, processResource());
        auto result =
            co_await tryRunBlocking(*pool, worker, [coding, plain = std::move(plain)]() mutable {
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
        if (attempt.status != BufferedCompressionAttemptStatus::kCompressed ||
            attempt.bytes.empty()) {
            co_return HttpResponseCompressionResult::makeFailed();
        }
        try {
            response.replaceBodyWithContentEncoding(
                std::move(attempt.bytes), httpContentCodingToken(coding));
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

bool prepareStreamingResponseCompression(const HttpResponseCodingSelection& selection,
    HttpKnownMethod requestMethod, HttpResponse& response, ResponseStreamKind kind) {
    if (selection.coding() == HttpContentCoding::kIdentity ||
        httpResponseCompressionEligibility(selection, requestMethod, response, kind) !=
            HttpResponseCompressionEligibility::kEligible) {
        return false;
    }

    response.addVaryToken("Accept-Encoding");
    response.applyContentEncoding(httpContentCodingToken(selection.coding()));
    return true;
}

}  // namespace ruvia::detail
