#pragma once

#include "../RequestMemoryArena.h"
#include "../RequestBodyLimit.h"
#include "../../http/HttpParserInternal.h"

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
    return parsed.flags.expectContinue;
}

}  // namespace ruvia::detail
