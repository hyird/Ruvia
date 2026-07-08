#pragma once

#include "HttpResponseBodyAccess.h"
#include "net/http1/Http1ServerSemantics.h"
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
        response.header("Retry-After", std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
    }
}

inline bool requestLimitReached(std::size_t requestCount, std::size_t maxRequests) noexcept {
    return maxRequests != 0 && requestCount >= maxRequests;
}

inline void applyRequestLimit(bool& keepAlive, std::size_t requestCount, std::size_t maxRequests) noexcept {
    if (requestLimitReached(requestCount, maxRequests)) {
        keepAlive = false;
    }
}

inline void markConnectionCloseAfterWrite(HttpResponse& response, bool& closeAfterWrite) {
    http1MarkConnectionClose(response);
    closeAfterWrite = true;
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
    std::size_t maxRequests,
    bool needsKeepAliveSignal) {
    if (http1ResponseWantsClose(response)) {
        keepAlive = false;
    }
    recordCompletedRequest(keepAlive, requestCount, maxRequests);
    http1MarkConnectionCloseIfNeeded(response, keepAlive);
    http1MarkConnectionKeepAliveIfNeeded(response, keepAlive, needsKeepAliveSignal);
}

inline void finalizeBodyRouteResponse(
    HttpResponse& response,
    bool& keepAlive,
    std::size_t& requestCount,
    std::size_t maxRequests,
    bool requestBodyComplete,
    bool needsKeepAliveSignal) {
    if (http1ResponseWantsClose(response) || !requestBodyComplete) {
        keepAlive = false;
    }
    recordCompletedRequest(keepAlive, requestCount, maxRequests);
    // Fix borrowed response views before callers restore pipeline bytes.
    materializeResponseBody(response);
    http1MarkConnectionCloseIfNeeded(response, keepAlive);
    http1MarkConnectionKeepAliveIfNeeded(response, keepAlive, needsKeepAliveSignal);
}

}  // namespace ruvia::detail
