#pragma once

#include <string>
#include <string_view>

#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

inline void setResponseHeaderIfMissing(
    HttpResponse& response,
    HttpResponse::KnownHeaderBit bit,
    std::string_view name,
    std::string_view value) {
    if (!response.hasKnownHeader(bit)) {
        response.setHeader(name, value);
    }
}

inline void addVaryToken(HttpResponse& response, std::string_view token) {
    const auto vary = response.header(HttpResponse::kKnownHeaderVary);
    if (vary.empty()) {
        response.setHeader("Vary", token);
        return;
    }
    if (httpHasToken(vary, token)) {
        return;
    }

    std::pmr::string updated(response.resource());
    updated.append(vary);
    updated.append(", ");
    updated.append(token.data(), token.size());
    response.setHeader("Vary", updated);
}

}  // namespace ruvia::detail
