#include "ruvia/http/HttpParseError.h"
#include "ruvia/http/detail/HttpRequestBodyFailure.h"

namespace ruvia {

HttpProtocolError httpParseProtocolError(HttpParseError error) noexcept {
    switch (error) {
        case HttpParseError::kHeaderTooLarge:
            return HttpProtocolError(http_status::kRequestHeaderFieldsTooLarge, "request header is too large");
        case HttpParseError::kBodyTooLarge:
            return detail::HttpRequestBodyFailure::tooLarge().protocolError();
        case HttpParseError::kInvalidRequestLine:
            return HttpProtocolError(http_status::kBadRequest, "invalid request line");
        case HttpParseError::kUnsupportedHttpVersion:
            return HttpProtocolError(http_status::kHttpVersionNotSupported, "unsupported HTTP version");
        case HttpParseError::kInvalidRequestTarget:
            return HttpProtocolError(http_status::kBadRequest, "invalid request target");
        case HttpParseError::kInvalidHeader:
            return HttpProtocolError(http_status::kBadRequest, "invalid request header");
        case HttpParseError::kInvalidConnection:
            return HttpProtocolError(http_status::kBadRequest, "invalid Connection header");
        case HttpParseError::kInvalidUpgrade:
            return HttpProtocolError(http_status::kBadRequest, "invalid Upgrade header");
        case HttpParseError::kTooManyHeaders:
            return HttpProtocolError(http_status::kRequestHeaderFieldsTooLarge, "too many request headers");
        case HttpParseError::kMissingHost:
            return HttpProtocolError(http_status::kBadRequest, "missing Host header");
        case HttpParseError::kInvalidHost:
            return HttpProtocolError(http_status::kBadRequest, "invalid Host header");
        case HttpParseError::kInvalidContentLength:
        case HttpParseError::kConflictingContentLength:
            return HttpProtocolError(http_status::kBadRequest, "invalid Content-Length header");
        case HttpParseError::kInvalidTransferEncoding:
            return HttpProtocolError(http_status::kBadRequest, "invalid Transfer-Encoding header");
        case HttpParseError::kUnsupportedTransferEncoding:
            return HttpProtocolError(http_status::kNotImplemented, "unsupported transfer encoding");
        case HttpParseError::kInvalidChunkSize:
            return HttpProtocolError(http_status::kBadRequest, "invalid chunk size");
        case HttpParseError::kChunkSizeOverflow:
            return HttpProtocolError(http_status::kBadRequest, "chunk size is too large");
        case HttpParseError::kInvalidChunkExtension:
            return HttpProtocolError(http_status::kBadRequest, "invalid chunk extension");
        case HttpParseError::kInvalidChunkCrlf:
            return HttpProtocolError(http_status::kBadRequest, "invalid chunk delimiter");
        case HttpParseError::kInvalidTrailer:
            return HttpProtocolError(http_status::kBadRequest, "invalid chunk trailer");
    }
    return HttpProtocolError(http_status::kBadRequest, "invalid HTTP request");
}

}  // namespace ruvia
