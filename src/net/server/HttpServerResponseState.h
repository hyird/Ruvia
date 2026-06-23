#pragma once

#include "../../http/HttpResponseBodyAccess.h"
#include "../../http/HttpResponseHeaderState.h"
#include "../../http/HeaderTokenUtils.h"
#include "ruvia/http/HttpTypes.h"

#include <charconv>
#include <chrono>
#include <cstddef>

namespace ruvia::detail {

// Sets Retry-After to the rate-limit window rounded up to whole seconds (min 1).
inline void setRetryAfterSeconds(HttpResponse& response, std::chrono::milliseconds window) {
    const auto ms = window.count();
    const auto seconds = ms <= 0 ? 1 : (ms + 999) / 1000;
    char buffer[20];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), seconds);
    if (ec == std::errc{}) {
        response.setHeader("Retry-After", std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
    }
}

inline bool responseWantsClose(const HttpResponse& response) noexcept {
    return detail::httpHasToken(responseKnownHeader(response, kResponseHeaderConnection), "close");
}

inline bool requestLimitReached(std::size_t requestCount, std::size_t maxRequests) noexcept {
    return maxRequests != 0 && requestCount >= maxRequests;
}

inline void applyRequestLimit(bool& keepAlive, std::size_t requestCount, std::size_t maxRequests) noexcept {
    if (requestLimitReached(requestCount, maxRequests)) {
        keepAlive = false;
    }
}

inline void markConnectionClose(HttpResponse& response) {
    setResponseHeaderStableView(response, "Connection", "close");
}

inline void markConnectionCloseAfterWrite(HttpResponse& response, bool& closeAfterWrite) {
    markConnectionClose(response);
    closeAfterWrite = true;
}

inline void markConnectionCloseIfNeeded(HttpResponse& response, bool keepAlive) {
    if (!keepAlive) {
        markConnectionClose(response);
    }
}

inline void recordCompletedRequest(
    bool& keepAlive,
    std::size_t& requestCount,
    std::size_t maxRequests) noexcept {
    ++requestCount;
    applyRequestLimit(keepAlive, requestCount, maxRequests);
}

inline void finalizeBufferedRouteResponse(
    HttpResponse& response,
    bool& keepAlive,
    std::size_t& requestCount,
    std::size_t maxRequests) {
    if (responseWantsClose(response)) {
        keepAlive = false;
    }
    recordCompletedRequest(keepAlive, requestCount, maxRequests);
    markConnectionCloseIfNeeded(response, keepAlive);
}

inline void finalizeBodyRouteResponse(
    HttpResponse& response,
    bool& keepAlive,
    std::size_t& requestCount,
    std::size_t maxRequests,
    bool requestBodyComplete) {
    if (responseWantsClose(response) || !requestBodyComplete) {
        keepAlive = false;
    }
    recordCompletedRequest(keepAlive, requestCount, maxRequests);
    // Fix borrowed response views before callers restore pipeline bytes.
    materializeResponseBody(response);
    markConnectionCloseIfNeeded(response, keepAlive);
}

}  // namespace ruvia::detail
