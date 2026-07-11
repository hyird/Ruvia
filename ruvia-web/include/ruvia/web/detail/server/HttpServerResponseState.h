#pragma once

#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
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

[[nodiscard]] inline Http1ServerConnectionPlan applyRequestLimit(
    Http1ServerConnectionPlan connectionPlan,
    std::size_t requestCount,
    std::size_t maxRequests) noexcept {
    return requestLimitReached(requestCount, maxRequests)
        ? connectionPlan.requireClose()
        : connectionPlan;
}

[[nodiscard]] inline Http1ServerClosePolicy nextHttp1ResponseClosePolicy(
    std::size_t completedRequests,
    std::size_t maxRequests) noexcept {
    return maxRequests != 0 && completedRequests >= maxRequests - 1
        ? Http1ServerClosePolicy::kCloseAfterResponse
        : Http1ServerClosePolicy::kAllowReuse;
}

[[nodiscard]] inline Http1ServerConnectionPlan finalizeBufferedRouteResponse(
    HttpResponse& response,
    Http1ServerConnectionPlan connectionPlan,
    std::size_t& requestCount,
    std::size_t maxRequests) {
    ++requestCount;
    connectionPlan = applyRequestLimit(connectionPlan, requestCount, maxRequests);
    return http1FinalizeResponseConnection(response, connectionPlan);
}

[[nodiscard]] inline Http1ServerConnectionPlan finalizeBodyRouteResponse(
    HttpResponse& response,
    Http1ServerConnectionPlan connectionPlan,
    std::size_t& requestCount,
    std::size_t maxRequests,
    Http1RequestBodyConsumption bodyConsumption) {
    connectionPlan = http1ApplyRequestBodyConsumption(connectionPlan, bodyConsumption);
    ++requestCount;
    connectionPlan = applyRequestLimit(connectionPlan, requestCount, maxRequests);
    // Fix borrowed response views before callers restore pipeline bytes.
    materializeResponseBody(response);
    return http1FinalizeResponseConnection(response, connectionPlan);
}

}  // namespace ruvia::detail
