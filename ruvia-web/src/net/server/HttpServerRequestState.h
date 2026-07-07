#pragma once

#include "net/RequestMemoryArena.h"
#include "net/RequestBodyLimit.h"
#include "HttpParserInternal.h"

namespace ruvia::detail {

inline bool contentLengthExceedsLimit(std::size_t contentLength, std::size_t limit) noexcept {
    return limit != 0 && contentLength > limit;
}

inline bool shouldKeepAlive(const HttpServerParseResult& parsed) noexcept {
    if (parsed.flags.connectionClose) {
        return false;
    }
    if (parsed.flags.connectionKeepAlive) {
        return true;
    }
    return parsed.request.httpVersion() == "HTTP/1.1";
}

inline bool wantsContinue(const HttpServerParseResult& parsed) noexcept {
    // Only HTTP/1.1 clients understand an interim 1xx response. RFC 9110 section 15.2
    // and RFC 7231 section 6.2 say a server MUST NOT send a 1xx response to an HTTP/1.0 client --
    // it has no notion of interim responses and would read the "100 Continue" as the
    // final response. So an Expect: 100-continue from HTTP/1.0 is ignored, not
    // honored with a 100.
    return parsed.flags.expectContinue && parsed.request.httpVersion() == "HTTP/1.1";
}

}  // namespace ruvia::detail
