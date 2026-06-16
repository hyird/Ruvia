#include "ruvia/http/HttpParser.h"

#include "HttpParserSyntax.h"
#include "ruvia/http/HeaderUtils.h"

namespace ruvia {
namespace {

[[nodiscard]] bool isForbiddenChunkTrailer(std::string_view name) noexcept {
    switch (detail::classifyRequestHeader(name)) {
        case detail::RequestHeaderKind::kHost:
        case detail::RequestHeaderKind::kContentLength:
        case detail::RequestHeaderKind::kTransferEncoding:
        case detail::RequestHeaderKind::kConnection:
        case detail::RequestHeaderKind::kContentType:
        case detail::RequestHeaderKind::kCookie:
        case detail::RequestHeaderKind::kExpect:
        case detail::RequestHeaderKind::kIfMatch:
        case detail::RequestHeaderKind::kIfModifiedSince:
        case detail::RequestHeaderKind::kIfNoneMatch:
        case detail::RequestHeaderKind::kIfRange:
        case detail::RequestHeaderKind::kIfUnmodifiedSince:
        case detail::RequestHeaderKind::kRange:
        case detail::RequestHeaderKind::kUpgrade:
        case detail::RequestHeaderKind::kAuthorization:
            return true;
        case detail::RequestHeaderKind::kOther:
        case detail::RequestHeaderKind::kAccept:
        case detail::RequestHeaderKind::kAcceptEncoding:
        case detail::RequestHeaderKind::kAccessControlRequestHeaders:
        case detail::RequestHeaderKind::kAccessControlRequestMethod:
        case detail::RequestHeaderKind::kUserAgent:
        case detail::RequestHeaderKind::kOrigin:
        case detail::RequestHeaderKind::kSecWebSocketKey:
        case detail::RequestHeaderKind::kSecWebSocketProtocol:
        case detail::RequestHeaderKind::kSecWebSocketVersion:
            break;
    }

    switch (name.size()) {
        case 2:
            return detail::httpAsciiEqualsIgnoreCase(name, "TE");
        case 7:
            return detail::httpAsciiEqualsIgnoreCase(name, "Trailer");
        case 10:
            return detail::httpAsciiEqualsIgnoreCase(name, "Keep-Alive") ||
                detail::httpAsciiEqualsIgnoreCase(name, "Set-Cookie");
        case 13:
            return detail::httpAsciiEqualsIgnoreCase(name, "Authorization") ||
                detail::httpAsciiEqualsIgnoreCase(name, "Cache-Control");
        case 14:
            return detail::httpAsciiEqualsIgnoreCase(name, "Max-Forwards");
        case 15:
            return detail::httpAsciiEqualsIgnoreCase(name, "Accept-Ranges");
        case 16:
            return detail::httpAsciiEqualsIgnoreCase(name, "Content-Range");
        case 17:
            return detail::httpAsciiEqualsIgnoreCase(name, "Content-Encoding");
        case 18:
            return detail::httpAsciiEqualsIgnoreCase(name, "Proxy-Authenticate");
        case 19:
            return detail::httpAsciiEqualsIgnoreCase(name, "Proxy-Authorization");
        default:
            return false;
    }
}

}  // namespace

HttpChunkScanStatus validateHttpChunkTrailers(std::string_view trailers) noexcept {
    if (trailers.empty()) {
        return HttpChunkScanStatus::kComplete;
    }

    std::size_t cursor = 0;
    while (cursor < trailers.size()) {
        const auto lineEnd = trailers.find("\r\n", cursor);
        const auto line = lineEnd == std::string_view::npos
            ? trailers.substr(cursor)
            : trailers.substr(cursor, lineEnd - cursor);
        if (line.empty() || line.front() == ' ' || line.front() == '\t') {
            return HttpChunkScanStatus::kInvalidTrailer;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return HttpChunkScanStatus::kInvalidTrailer;
        }
        const auto name = line.substr(0, colon);
        const auto value = detail::httpTrimOws(line.substr(colon + 1));
        if (!detail::isValidHttpHeaderName(name) ||
            !detail::isValidHttpHeaderValue(value) ||
            isForbiddenChunkTrailer(name)) {
            return HttpChunkScanStatus::kInvalidTrailer;
        }
        if (lineEnd == std::string_view::npos) {
            return HttpChunkScanStatus::kComplete;
        }
        cursor = lineEnd + 2;
    }
    return HttpChunkScanStatus::kComplete;
}

bool parseHttpChunkSize(std::string_view value, std::size_t& size) noexcept {
    return detail::parseHttpChunkSizeLine(value, size) == detail::ChunkSizeLineStatus::kOk;
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
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kIncomplete};
        }

        std::size_t chunkSize = 0;
        switch (detail::parseHttpChunkSizeLine(body.substr(cursor, lineEnd - cursor), chunkSize)) {
            case detail::ChunkSizeLineStatus::kOk:
                break;
            case detail::ChunkSizeLineStatus::kInvalidSize:
                return HttpChunkScanResult{.status = HttpChunkScanStatus::kInvalidSize};
            case detail::ChunkSizeLineStatus::kOverflow:
                return HttpChunkScanResult{.status = HttpChunkScanStatus::kSizeOverflow};
            case detail::ChunkSizeLineStatus::kInvalidExtension:
                return HttpChunkScanResult{.status = HttpChunkScanStatus::kInvalidExtension};
        }
        if (!addOverhead(lineEnd - cursor + 2)) {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kTooLarge};
        }
        cursor = lineEnd + 2;

        if (chunkSize == 0) {
            if (body.substr(cursor, 2) == "\r\n") {
                if (!addOverhead(2)) {
                    return HttpChunkScanResult{.status = HttpChunkScanStatus::kTooLarge};
                }
                return HttpChunkScanResult{
                    .status = HttpChunkScanStatus::kComplete,
                    .consumedBytes = cursor + 2,
                    .decodedBytes = decoded};
            }
            const auto trailerEnd = body.find("\r\n\r\n", cursor);
            if (trailerEnd != std::string_view::npos) {
                if (const auto trailerStatus = validateHttpChunkTrailers(body.substr(cursor, trailerEnd - cursor));
                    trailerStatus != HttpChunkScanStatus::kComplete) {
                    return HttpChunkScanResult{.status = trailerStatus};
                }
                if (!addOverhead(trailerEnd - cursor + 4)) {
                    return HttpChunkScanResult{.status = HttpChunkScanStatus::kTooLarge};
                }
                return HttpChunkScanResult{
                    .status = HttpChunkScanStatus::kComplete,
                    .consumedBytes = trailerEnd + 4,
                    .decodedBytes = decoded};
            }
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kIncomplete};
        }

        if (chunkSize > kMaxHttpBodyBytes || decoded > kMaxHttpBodyBytes - chunkSize) {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kTooLarge};
        }
        if (body.size() < cursor + chunkSize + 2) {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kIncomplete};
        }
        if (body.substr(cursor + chunkSize, 2) != "\r\n") {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kInvalidCrlf};
        }
        if (!addOverhead(2)) {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kTooLarge};
        }

        decoded += chunkSize;
        cursor += chunkSize + 2;
    }
}

}  // namespace ruvia
