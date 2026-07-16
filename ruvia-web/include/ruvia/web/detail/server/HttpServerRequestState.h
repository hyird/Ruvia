#pragma once

#include <optional>

#include "ruvia/web/detail/server/RequestMemoryArena.h"
#include "ruvia/web/detail/server/RequestBodyLimit.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/http/detail/HttpRequestBodyFailure.h"

namespace ruvia::detail {

inline std::optional<HttpRequestBodyFailure> contentLengthLimitFailure(
    const Http1RequestBodyPlan& bodyPlan,
    ProtocolByteLimit limit) noexcept {
    const auto* knownLength = bodyPlan.knownLength();
    return knownLength == nullptr
        ? std::nullopt
        : httpRequestBodySizeFailure(knownLength->contentLength(), limit);
}

}  // namespace ruvia::detail
