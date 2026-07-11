#pragma once

#include "ruvia/web/detail/server/RequestMemoryArena.h"
#include "ruvia/web/detail/server/RequestBodyLimit.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"

namespace ruvia::detail {

inline bool contentLengthExceedsLimit(
    const Http1RequestBodyPlan& bodyPlan,
    std::size_t limit) noexcept {
    const auto* knownLength = bodyPlan.knownLength();
    return knownLength != nullptr &&
        limit != 0 &&
        knownLength->contentLength() > limit;
}

}  // namespace ruvia::detail
