#include "ruvia/http/detail/parser/HttpChunkParser.h"

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool isForbiddenChunkTrailer(std::string_view name) noexcept {
    switch (classifyRequestHeader(name)) {
        case RequestHeaderKind::kHost:
        case RequestHeaderKind::kContentLength:
        case RequestHeaderKind::kTransferEncoding:
        case RequestHeaderKind::kConnection:
        case RequestHeaderKind::kContentEncoding:
        case RequestHeaderKind::kContentType:
        case RequestHeaderKind::kCookie:
        case RequestHeaderKind::kExpect:
        case RequestHeaderKind::kIfMatch:
        case RequestHeaderKind::kIfModifiedSince:
        case RequestHeaderKind::kIfNoneMatch:
        case RequestHeaderKind::kIfRange:
        case RequestHeaderKind::kIfUnmodifiedSince:
        case RequestHeaderKind::kRange:
        case RequestHeaderKind::kUpgrade:
        case RequestHeaderKind::kAuthorization:
        case RequestHeaderKind::kAccessControlRequestHeaders:
        case RequestHeaderKind::kAccessControlRequestMethod:
        case RequestHeaderKind::kOrigin:
            return true;
        case RequestHeaderKind::kOther:
        case RequestHeaderKind::kAccept:
        case RequestHeaderKind::kAcceptEncoding:
        case RequestHeaderKind::kUserAgent:
        case RequestHeaderKind::kSecWebSocketKey:
        case RequestHeaderKind::kSecWebSocketProtocol:
        case RequestHeaderKind::kSecWebSocketVersion:
            break;
    }

    switch (name.size()) {
        case 2:
            return httpAsciiEqualsIgnoreCase(name, "TE");
        case 7:
            return httpAsciiEqualsIgnoreCase(name, "Trailer");
        case 10:
            return httpAsciiEqualsIgnoreCase(name, "Keep-Alive") ||
                httpAsciiEqualsIgnoreCase(name, "Set-Cookie");
        case 12:
            return httpAsciiEqualsIgnoreCase(name, "Max-Forwards");
        case 13:
            // The common classified forbidden fields are caught above; keep the
            // less common trailer-forbidden names here without growing the hot
            // request known-header table.
            return httpAsciiEqualsIgnoreCase(name, "Cache-Control") ||
                httpAsciiEqualsIgnoreCase(name, "Accept-Ranges") ||
                httpAsciiEqualsIgnoreCase(name, "Content-Range");
        case 16:
            // Proxy-Connection completes the connection-specific set that must not
            // arrive late in a trailer (Connection / Keep-Alive / Transfer-Encoding
            // / Upgrade are all rejected above): a non-standard but widely honored
            // hop-by-hop control. Over HTTP/1 the trailer check is the only guard,
            // exactly as for Upgrade; the HTTP/2 path already bans it as a
            // connection-specific header. Content-Encoding is also caught by the
            // classified path above and is repeated here only defensively.
            return httpAsciiEqualsIgnoreCase(name, "Content-Encoding") ||
                httpAsciiEqualsIgnoreCase(name, "Proxy-Connection");
        case 18:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Authenticate");
        case 19:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Authorization");
        default:
            return false;
    }
}

}  // namespace

std::optional<HttpChunkScanError> validateHttpChunkTrailers(
    std::string_view trailers) noexcept {
    if (trailers.empty()) {
        return std::nullopt;
    }
    if (trailers.size() > kMaxHttpHeaderBytes) {
        return HttpChunkScanError::kTooLarge;
    }

    std::size_t cursor = 0;
    std::size_t fieldCount = 0;
    while (cursor < trailers.size()) {
        if (fieldCount == kMaxHttpHeaderFields) {
            return HttpChunkScanError::kTooLarge;
        }
        ++fieldCount;
        const auto lineEnd = trailers.find("\r\n", cursor);
        const auto line = lineEnd == std::string_view::npos
            ? trailers.substr(cursor)
            : trailers.substr(cursor, lineEnd - cursor);
        if (line.empty() || line.front() == ' ' || line.front() == '\t') {
            return HttpChunkScanError::kInvalidTrailer;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return HttpChunkScanError::kInvalidTrailer;
        }
        const auto name = line.substr(0, colon);
        const auto value = httpTrimOws(line.substr(colon + 1));
        if (!isValidHttpHeaderName(name) ||
            !isValidHttpHeaderValue(value) ||
            isForbiddenChunkTrailer(name)) {
            return HttpChunkScanError::kInvalidTrailer;
        }
        if (lineEnd == std::string_view::npos) {
            return std::nullopt;
        }
        cursor = lineEnd + 2;
    }
    return std::nullopt;
}

bool parseHttpChunkSize(std::string_view value, std::size_t& size) noexcept {
    return parseHttpChunkSizeLine(value, size) == ChunkSizeLineStatus::kOk;
}

HttpChunkScanResult scanHttpChunkedBody(std::string_view body) noexcept {
    std::size_t cursor = 0;
    std::size_t decoded = 0;
    std::size_t encodedOverhead = 0;
    const auto addOverhead = [&encodedOverhead](std::size_t bytes) noexcept {
        if (bytes > kMaxHttpBodyBytes || encodedOverhead > kMaxHttpBodyBytes - bytes) {
            return false;
        }
        encodedOverhead += bytes;
        return true;
    };
    for (;;) {
        const auto lineEnd = body.find("\r\n", cursor);
        if (lineEnd == std::string_view::npos) {
            // A future CRLF can begin at most one byte before the current end.
            // Once the unterminated line itself reaches the framing-line byte
            // limit, no completion can make this a bounded chunk-size line.
            if (body.size() - cursor >= kMaxHttpHeaderBytes) {
                return HttpChunkScanResult::makeFailure(
                    HttpChunkScanError::kTooLarge);
            }
            return HttpChunkScanResult::makeNeedMore();
        }
        if (lineEnd - cursor + 2 > kMaxHttpHeaderBytes) {
            return HttpChunkScanResult::makeFailure(
                HttpChunkScanError::kTooLarge);
        }

        std::size_t chunkSize = 0;
        switch (parseHttpChunkSizeLine(body.substr(cursor, lineEnd - cursor), chunkSize)) {
            case ChunkSizeLineStatus::kOk:
                break;
            case ChunkSizeLineStatus::kInvalidSize:
                return HttpChunkScanResult::makeFailure(
                    HttpChunkScanError::kInvalidSize);
            case ChunkSizeLineStatus::kOverflow:
                return HttpChunkScanResult::makeFailure(
                    HttpChunkScanError::kSizeOverflow);
            case ChunkSizeLineStatus::kInvalidExtension:
                return HttpChunkScanResult::makeFailure(
                    HttpChunkScanError::kInvalidExtension);
        }
        if (!addOverhead(lineEnd - cursor + 2)) {
            return HttpChunkScanResult::makeFailure(
                HttpChunkScanError::kTooLarge);
        }
        cursor = lineEnd + 2;

        if (chunkSize == 0) {
            if (body.substr(cursor, 2) == "\r\n") {
                if (!addOverhead(2)) {
                    return HttpChunkScanResult::makeFailure(
                        HttpChunkScanError::kTooLarge);
                }
                return HttpChunkScanResult::makeComplete(cursor + 2);
            }
            const auto trailerEnd = body.find("\r\n\r\n", cursor);
            if (trailerEnd != std::string_view::npos) {
                if (trailerEnd - cursor > kMaxHttpHeaderBytes - 4) {
                    return HttpChunkScanResult::makeFailure(
                        HttpChunkScanError::kTooLarge);
                }
                if (const auto trailerError = validateHttpChunkTrailers(
                        body.substr(cursor, trailerEnd - cursor));
                    trailerError.has_value()) {
                    return HttpChunkScanResult::makeFailure(*trailerError);
                }
                if (!addOverhead(trailerEnd - cursor + 4)) {
                    return HttpChunkScanResult::makeFailure(
                        HttpChunkScanError::kTooLarge);
                }
                return HttpChunkScanResult::makeComplete(trailerEnd + 4);
            }
            // Preserve the last three bytes as a possible delimiter prefix.
            // At kMaxHttpHeaderBytes available bytes, even the earliest future
            // CRLFCRLF would exceed the complete trailer-section limit.
            if (body.size() - cursor >= kMaxHttpHeaderBytes) {
                return HttpChunkScanResult::makeFailure(
                    HttpChunkScanError::kTooLarge);
            }
            return HttpChunkScanResult::makeNeedMore();
        }

        if (chunkSize > kMaxHttpBodyBytes || decoded > kMaxHttpBodyBytes - chunkSize) {
            return HttpChunkScanResult::makeFailure(
                HttpChunkScanError::kTooLarge);
        }
        if (body.size() < cursor + chunkSize + 2) {
            return HttpChunkScanResult::makeNeedMore();
        }
        if (body.substr(cursor + chunkSize, 2) != "\r\n") {
            return HttpChunkScanResult::makeFailure(
                HttpChunkScanError::kInvalidCrlf);
        }
        if (!addOverhead(2)) {
            return HttpChunkScanResult::makeFailure(
                HttpChunkScanError::kTooLarge);
        }

        decoded += chunkSize;
        cursor += chunkSize + 2;
    }
}

}  // namespace ruvia::detail
