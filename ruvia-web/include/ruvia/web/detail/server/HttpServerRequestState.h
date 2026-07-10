#pragma once

#include "ruvia/web/detail/server/RequestMemoryArena.h"
#include "ruvia/web/detail/server/RequestBodyLimit.h"
#include "ruvia/http/detail/HttpParserInternal.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"

namespace ruvia::detail {

inline bool contentLengthExceedsLimit(std::size_t contentLength, std::size_t limit) noexcept {
    return limit != 0 && contentLength > limit;
}

}  // namespace ruvia::detail
