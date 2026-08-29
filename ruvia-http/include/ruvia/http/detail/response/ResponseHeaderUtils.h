#pragma once

#include <algorithm>
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

inline bool setKnownStaticVaryToken(HttpResponse& response, std::string_view token) {
    if (token == "Accept-Encoding" || token == "Origin" ||
        token == "Access-Control-Request-Headers" || token == "Access-Control-Request-Method") {
        setResponseHeaderStableView(response, "Vary", token);
        return true;
    }
    return false;
}

inline bool varyTokenRepeatedInBatch(const std::string_view* tokens, std::size_t current) noexcept {
    return std::ranges::any_of(
        std::span(tokens, current), [candidate = tokens[current]](std::string_view token) noexcept {
            return httpAsciiEqualsIgnoreCase(token, candidate);
        });
}

[[nodiscard]] inline bool responseVaryHasToken(
    const HttpResponse& response, std::string_view token) noexcept {
    if (!responseHasKnownHeader(response, kResponseHeaderVary)) {
        return false;
    }
    for (const auto& header : response.headers()) {
        if (responseHeaderKnownBit(header) == kResponseHeaderVary &&
            httpHasToken(header.value(), token)) {
            return true;
        }
    }
    return false;
}

inline void setResponseHeaderIfMissing(
    HttpResponse& response, std::uint32_t bit, std::string_view name, std::string_view value) {
    if (!responseHasKnownHeader(response, bit)) {
        response.header(name, value);
    }
}

inline void setStableResponseHeaderIfMissing(
    HttpResponse& response, std::uint32_t bit, std::string_view name, std::string_view value) {
    if (!responseHasKnownHeader(response, bit)) {
        setResponseHeaderStableView(response, name, value);
    }
}

inline void addVaryTokens(
    HttpResponse& response, const std::string_view* tokens, std::size_t tokenCount) {
    if (tokens == nullptr || tokenCount == 0) {
        return;
    }

    // RFC 9110 sections 5.2-5.3 define repeated Vary field lines as one
    // comma-joined value in wire order. Inspect every line: the O(1) known-header
    // lookup intentionally returns only the first occurrence and cannot decide
    // wildcard or token membership for this list-based field.
    if (responseVaryHasToken(response, "*")) {
        return;
    }
    for (std::size_t i = 0; i < tokenCount; ++i) {
        if (httpTrimOws(tokens[i]) == "*") {
            setResponseHeaderStableView(response, "Vary", "*");
            return;
        }
    }
    const bool useAddMask = tokenCount <= 64;
    std::uint64_t addMask = 0;
    std::size_t addedCount = 0;
    std::size_t addedBytes = 0;
    std::string_view firstAdded;
    for (std::size_t i = 0; i < tokenCount; ++i) {
        const auto token = tokens[i];
        if (token.empty() || responseVaryHasToken(response, token) ||
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

    std::size_t existingValueCount = 0;
    std::size_t existingBytes = 0;
    if (responseHasKnownHeader(response, kResponseHeaderVary)) {
        for (const auto& header : response.headers()) {
            if (responseHeaderKnownBit(header) != kResponseHeaderVary ||
                httpTrimOws(header.value()).empty()) {
                continue;
            }
            ++existingValueCount;
            existingBytes += header.value().size();
        }
    }

    if (existingValueCount == 0) {
        if (addedCount == 1 && setKnownStaticVaryToken(response, firstAdded)) {
            return;
        }
    }

    std::pmr::string updated(responseResource(response));
    const auto partCount = existingValueCount + addedCount;
    updated.reserve(existingBytes + addedBytes + (partCount == 0 ? 0 : (partCount - 1) * 2));
    if (existingValueCount != 0) {
        for (const auto& header : response.headers()) {
            if (responseHeaderKnownBit(header) != kResponseHeaderVary ||
                httpTrimOws(header.value()).empty()) {
                continue;
            }
            if (!updated.empty()) {
                updated.append(", ");
            }
            updated.append(header.value());
        }
    }
    for (std::size_t i = 0; i < tokenCount; ++i) {
        const auto token = tokens[i];
        if (useAddMask) {
            if ((addMask & (std::uint64_t{1} << i)) == 0) {
                continue;
            }
        } else {
            if (token.empty() || responseVaryHasToken(response, token) ||
                varyTokenRepeatedInBatch(tokens, i)) {
                continue;
            }
        }
        if (!updated.empty()) {
            updated.append(", ");
        }
        updated.append(token.data(), token.size());
    }
    response.header("Vary", updated);
}

inline void addVaryToken(HttpResponse& response, std::string_view token) {
    // A single token is exactly a one-element batch; delegate so the dedup,
    // static-token fast path, and precise-reserve logic live in one place.
    addVaryTokens(response, &token, 1);
}

}  // namespace ruvia::detail
