#include "ruvia/http/HttpParseError.h"
#include "ruvia/http/detail/HttpRequestBodyFailure.h"

namespace ruvia {

HttpProtocolError httpParseProtocolError(HttpParseError error) noexcept {
    switch (error) {
        case HttpParseError::kHeaderTooLarge:
            return HttpProtocolError(431, "request header is too large");
        case HttpParseError::kBodyTooLarge:
            return detail::HttpRequestBodyFailure::tooLarge().protocolError();
        case HttpParseError::kInvalidRequestLine:
            return HttpProtocolError(400, "invalid request line");
        case HttpParseError::kUnsupportedHttpVersion:
            return HttpProtocolError(505, "unsupported HTTP version");
        case HttpParseError::kInvalidRequestTarget:
            return HttpProtocolError(400, "invalid request target");
        case HttpParseError::kInvalidHeader:
            return HttpProtocolError(400, "invalid request header");
        case HttpParseError::kInvalidConnection:
            return HttpProtocolError(400, "invalid Connection header");
        case HttpParseError::kInvalidUpgrade:
            return HttpProtocolError(400, "invalid Upgrade header");
        case HttpParseError::kTooManyHeaders:
            return HttpProtocolError(431, "too many request headers");
        case HttpParseError::kMissingHost:
            return HttpProtocolError(400, "missing Host header");
        case HttpParseError::kInvalidHost:
            return HttpProtocolError(400, "invalid Host header");
        case HttpParseError::kInvalidContentLength:
        case HttpParseError::kConflictingContentLength:
            return HttpProtocolError(400, "invalid Content-Length header");
        case HttpParseError::kInvalidTransferEncoding:
            return HttpProtocolError(400, "invalid Transfer-Encoding header");
        case HttpParseError::kUnsupportedTransferEncoding:
            return HttpProtocolError(501, "unsupported transfer encoding");
        case HttpParseError::kInvalidChunkSize:
            return HttpProtocolError(400, "invalid chunk size");
        case HttpParseError::kChunkSizeOverflow:
            return HttpProtocolError(400, "chunk size is too large");
        case HttpParseError::kInvalidChunkExtension:
            return HttpProtocolError(400, "invalid chunk extension");
        case HttpParseError::kInvalidChunkCrlf:
            return HttpProtocolError(400, "invalid chunk delimiter");
        case HttpParseError::kInvalidTrailer:
            return HttpProtocolError(400, "invalid chunk trailer");
    }
    return HttpProtocolError(400, "invalid HTTP request");
}

}  // namespace ruvia
