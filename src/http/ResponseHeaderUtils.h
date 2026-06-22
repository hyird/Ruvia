#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "HttpResponseHeaderState.h"
#include "HeaderTokenUtils.h"
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
    std::uint32_t bit,
    std::string_view name,
    std::string_view value) {
    if (!responseHasKnownHeader(response, bit)) {
        response.setHeader(name, value);
    }
}

inline void setStableResponseHeaderIfMissing(
    HttpResponse& response,
    std::uint32_t bit,
    std::string_view name,
    std::string_view value) {
    if (!responseHasKnownHeader(response, bit)) {
        setResponseHeaderStableView(response, name, value);
    }
}

inline void addVaryTokens(
    HttpResponse& response,
    const std::string_view* tokens,
    std::size_t tokenCount) {
    if (tokens == nullptr || tokenCount == 0) {
        return;
    }

    const auto vary = responseKnownHeader(response, kResponseHeaderVary);
    const bool useAddMask = tokenCount <= 64;
    std::uint64_t addMask = 0;
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
        if (useAddMask) {
            addMask |= std::uint64_t{1} << i;
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

    std::pmr::string updated(responseResource(response));
    updated.reserve(vary.size() + addedBytes + (vary.empty() ? (addedCount - 1) : addedCount) * 2);
    if (!vary.empty()) {
        updated.append(vary);
    }
    for (std::size_t i = 0; i < tokenCount; ++i) {
        const auto token = tokens[i];
        if (useAddMask) {
            if ((addMask & (std::uint64_t{1} << i)) == 0) {
                continue;
            }
        } else {
            if (token.empty() ||
                (!vary.empty() && httpHasToken(vary, token)) ||
                varyTokenRepeatedInBatch(tokens, i)) {
                continue;
            }
        }
        if (!updated.empty()) {
            updated.append(", ");
        }
        updated.append(token.data(), token.size());
    }
    response.setHeader("Vary", updated);
}

inline void addVaryToken(HttpResponse& response, std::string_view token) {
    if (token.empty()) {
        return;
    }

    const auto vary = responseKnownHeader(response, kResponseHeaderVary);
    if (!vary.empty() && httpHasToken(vary, token)) {
        return;
    }
    if (vary.empty() && setKnownStaticVaryToken(response, token)) {
        return;
    }

    std::pmr::string updated(responseResource(response));
    updated.reserve(vary.size() + token.size() + (vary.empty() ? 0 : 2));
    if (!vary.empty()) {
        updated.append(vary);
        updated.append(", ");
    }
    updated.append(token.data(), token.size());
    response.setHeader("Vary", updated);
}

}  // namespace ruvia::detail
