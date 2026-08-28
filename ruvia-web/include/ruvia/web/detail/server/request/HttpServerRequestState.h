#pragma once

#include <optional>
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/request/HttpRequestBodyFailure.h"

namespace ruvia::detail {

inline std::optional<HttpRequestBodyFailure> contentLengthLimitFailure(const Http1RequestBodyPlan& bodyPlan, ProtocolByteLimit limit) noexcept {
    const auto* knownLength = bodyPlan.knownLength();
    return knownLength == nullptr ? std::nullopt : httpRequestBodySizeFailure(knownLength->contentLength(), limit);
}

}  // namespace ruvia::detail
