#include "ruvia/http/HttpParser.h"

namespace ruvia {

std::string_view httpParseErrorMessage(HttpParseError error) noexcept {
    switch (error) {
        case HttpParseError::kNone:
            return "no parse error";
        case HttpParseError::kHeaderTooLarge:
            return "request header is too large";
        case HttpParseError::kBodyTooLarge:
            return "request body is too large";
        case HttpParseError::kInvalidRequestLine:
            return "invalid request line";
        case HttpParseError::kUnsupportedHttpVersion:
            return "unsupported HTTP version";
        case HttpParseError::kInvalidRequestTarget:
            return "invalid request target";
        case HttpParseError::kUnsupportedMethod:
            return "unsupported request method";
        case HttpParseError::kInvalidHeader:
            return "invalid request header";
        case HttpParseError::kTooManyHeaders:
            return "too many request headers";
        case HttpParseError::kMissingHost:
            return "missing Host header";
        case HttpParseError::kInvalidHost:
            return "invalid Host header";
        case HttpParseError::kInvalidContentLength:
        case HttpParseError::kConflictingContentLength:
            return "invalid Content-Length header";
        case HttpParseError::kInvalidTransferEncoding:
            return "invalid Transfer-Encoding header";
        case HttpParseError::kUnsupportedTransferEncoding:
            return "unsupported transfer encoding";
        case HttpParseError::kExpectationFailed:
            return "unsupported Expect header";
        case HttpParseError::kInvalidChunkSize:
            return "invalid chunk size";
        case HttpParseError::kChunkSizeOverflow:
            return "chunk size is too large";
        case HttpParseError::kInvalidChunkExtension:
            return "invalid chunk extension";
        case HttpParseError::kInvalidChunkCrlf:
            return "invalid chunk delimiter";
        case HttpParseError::kInvalidTrailer:
            return "invalid chunk trailer";
    }

    return "invalid HTTP request";
}

std::uint16_t httpParseErrorStatus(HttpParseError error) noexcept {
    switch (error) {
        case HttpParseError::kHeaderTooLarge:
        case HttpParseError::kTooManyHeaders:
            return 431;
        case HttpParseError::kBodyTooLarge:
            return 413;
        case HttpParseError::kUnsupportedMethod:
        case HttpParseError::kUnsupportedTransferEncoding:
            return 501;
        case HttpParseError::kUnsupportedHttpVersion:
            return 505;
        case HttpParseError::kExpectationFailed:
            return 417;
        case HttpParseError::kNone:
        case HttpParseError::kInvalidRequestLine:
        case HttpParseError::kInvalidHeader:
        case HttpParseError::kInvalidRequestTarget:
        case HttpParseError::kMissingHost:
        case HttpParseError::kInvalidHost:
        case HttpParseError::kInvalidContentLength:
        case HttpParseError::kConflictingContentLength:
        case HttpParseError::kInvalidTransferEncoding:
        case HttpParseError::kInvalidChunkSize:
        case HttpParseError::kChunkSizeOverflow:
        case HttpParseError::kInvalidChunkExtension:
        case HttpParseError::kInvalidChunkCrlf:
        case HttpParseError::kInvalidTrailer:
            return 400;
    }

    return 400;
}

}  // namespace ruvia
