#include "ruvia/web/detail/server/response/HttpResponseCompression.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
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

HttpResponseCompressionDecision prepareResponseCompression(
    const HttpResponseCodingSelection& selection, HttpKnownMethod requestMethod,
    HttpResponse& response, HttpResponseCompressionSource source,
    HttpResponseCodingAvailability availability) {
    switch (source) {
        case HttpResponseCompressionSource::kBuffered:
        case HttpResponseCompressionSource::kStream:
        case HttpResponseCompressionSource::kSse:
            break;
        default:
            throw std::invalid_argument("invalid response compression source");
    }
    switch (availability) {
        case HttpResponseCodingAvailability::kIdentityOnly:
        case HttpResponseCodingAvailability::kIdentityAndCompression:
            break;
        default:
            throw std::invalid_argument("invalid response coding availability");
    }

    if (source == HttpResponseCompressionSource::kBuffered && response.fileBody().has_value()) {
        return HttpResponseCompressionDecision::kFixedRepresentation;
    }
    if (!planHttpResponseBody(requestMethod, response.status()).statusAllowsBody()) {
        return HttpResponseCompressionDecision::kFixedRepresentation;
    }
    const auto statusCode = response.status();
    if (statusCode == http_status::kPartialContent || statusCode == http_status::kResetContent ||
        responseHasHeaderName(response, "Content-Encoding") ||
        responseHasHeaderName(response, "Content-Range") ||
        (source != HttpResponseCompressionSource::kSse &&
            responseContentTypeSkipsCompression(
                response.header("Content-Type").value_or(std::string_view{}))) ||
        responseCacheControl(response).has(CacheControlDirective::kNoTransform) ||
        availability == HttpResponseCodingAvailability::kIdentityOnly) {
        return HttpResponseCompressionDecision::kFixedRepresentation;
    }

    response.addVaryToken("Accept-Encoding");
    if (selection.coding() == HttpContentCoding::kIdentity) {
        return HttpResponseCompressionDecision::kNegotiatedIdentity;
    }
    return HttpResponseCompressionDecision::kEncode;
}

HttpResponseCompressionResult applyResponseCompression(const HttpResponseCodingSelection& selection,
    HttpKnownMethod requestMethod, HttpResponse& response, const CompressionConfig& options) {
    const auto responseContent = response.bodyBytes();

    const auto decision = prepareResponseCompression(selection, requestMethod, response,
        HttpResponseCompressionSource::kBuffered,
        HttpResponseCodingAvailability::kIdentityAndCompression);
    if (decision != HttpResponseCompressionDecision::kEncode) {
        return HttpResponseCompressionResult::makeNotApplicable();
    }

    const auto coding = selection.coding();
    if (responseContent.size() < options.minBytes ||
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
    const auto decision = prepareResponseCompression(selection, requestMethod, response,
        HttpResponseCompressionSource::kBuffered,
        HttpResponseCodingAvailability::kIdentityAndCompression);
    if (decision != HttpResponseCompressionDecision::kEncode || size < options.minBytes ||
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

}  // namespace ruvia::detail
