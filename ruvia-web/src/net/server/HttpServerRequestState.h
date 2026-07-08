#pragma once

#include "net/RequestMemoryArena.h"
#include "net/RequestBodyLimit.h"
#include "HttpParserInternal.h"
#include "net/http1/Http1ServerSemantics.h"

namespace ruvia::detail {

inline bool contentLengthExceedsLimit(std::size_t contentLength, std::size_t limit) noexcept {
    return limit != 0 && contentLength > limit;
}

}  // namespace ruvia::detail
