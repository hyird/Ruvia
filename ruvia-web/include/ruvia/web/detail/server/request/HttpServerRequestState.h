#pragma once

#include <optional>

#include "ruvia/http/HttpRequestBodyFailure.h"
#include "ruvia/http/Http1ServerRequestParser.h"

namespace ruvia::detail {

inline std::optional<HttpRequestBodyFailure> contentLengthLimitFailure(
    const Http1RequestBodyPlan& bodyPlan, ProtocolByteLimit limit) noexcept {
    const auto* knownLength = bodyPlan.knownLength();
    return knownLength == nullptr ? std::nullopt
                                  : httpRequestBodySizeFailure(knownLength->contentLength(), limit);
}

}  // namespace ruvia::detail
