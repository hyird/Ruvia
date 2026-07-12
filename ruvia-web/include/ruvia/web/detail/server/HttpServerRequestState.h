#pragma once

#include "ruvia/web/detail/server/RequestMemoryArena.h"
#include "ruvia/web/detail/server/RequestBodyLimit.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"

namespace ruvia::detail {

inline bool contentLengthExceedsLimit(
    const Http1RequestBodyPlan& bodyPlan,
    HttpBodyByteLimit limit) noexcept {
    const auto* knownLength = bodyPlan.knownLength();
    return knownLength != nullptr &&
        limit.exceeds(knownLength->contentLength());
}

}  // namespace ruvia::detail
