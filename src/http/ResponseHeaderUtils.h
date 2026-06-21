#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

inline bool setKnownStaticVaryToken(HttpResponse& response, std::string_view token) {
    if (token == "Accept-Encoding" ||
        token == "Origin" ||
        token == "Access-Control-Request-Headers" ||
        token == "Access-Control-Request-Method") {
        setResponseHeaderStableView(response, "Vary", token);
        return true;
    }
    return false;
}

inline bool varyTokenRepeatedInBatch(
    const std::string_view* tokens,
    std::size_t current) noexcept {
    for (std::size_t i = 0; i < current; ++i) {
        if (httpAsciiEqualsIgnoreCase(tokens[i], tokens[current])) {
            return true;
        }
    }
    return false;
}

inline void setResponseHeaderIfMissing(
    HttpResponse& response,
    HttpResponse::KnownHeaderBit bit,
    std::string_view name,
    std::string_view value) {
    if (!response.hasKnownHeader(bit)) {
        response.setHeader(name, value);
    }
}

inline void addVaryTokens(
    HttpResponse& response,
    const std::string_view* tokens,
    std::size_t tokenCount) {
    if (tokens == nullptr || tokenCount == 0) {
        return;
    }

    const auto vary = response.header(HttpResponse::kKnownHeaderVary);
    std::size_t addedCount = 0;
    std::size_t addedBytes = 0;
    std::string_view firstAdded;
    for (std::size_t i = 0; i < tokenCount; ++i) {
        const auto token = tokens[i];
        if (token.empty() ||
            (!vary.empty() && httpHasToken(vary, token)) ||
            varyTokenRepeatedInBatch(tokens, i)) {
            continue;
        }
        if (addedCount == 0) {
            firstAdded = token;
        }
        ++addedCount;
        addedBytes += token.size();
    }
    if (addedCount == 0) {
        return;
    }

    if (vary.empty()) {
        if (addedCount == 1 && setKnownStaticVaryToken(response, firstAdded)) {
            return;
        }
    }

    std::pmr::string updated(response.resource());
    updated.reserve(vary.size() + addedBytes + (vary.empty() ? (addedCount - 1) : addedCount) * 2);
    if (!vary.empty()) {
        updated.append(vary);
    }
    for (std::size_t i = 0; i < tokenCount; ++i) {
        const auto token = tokens[i];
        if (token.empty() ||
            (!vary.empty() && httpHasToken(vary, token)) ||
            varyTokenRepeatedInBatch(tokens, i)) {
            continue;
        }
        if (!updated.empty()) {
            updated.append(", ");
        }
        updated.append(token.data(), token.size());
    }
    response.setHeader("Vary", updated);
}

inline void addVaryToken(HttpResponse& response, std::string_view token) {
    addVaryTokens(response, &token, 1);
}

}  // namespace ruvia::detail
