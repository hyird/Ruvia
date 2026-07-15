#pragma once

#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/detail/server/Http1RequestSequence.h"

#include <charconv>
#include <chrono>
#include <cstddef>

namespace ruvia::detail {

[[nodiscard]] inline Http1ServerConnectionPlan requireHttp1FinalResponseCommit(
    HttpResponse& response,
    Http1ServerConnectionPlan connectionPlan) {
    const auto result = http1CommitFinalResponse(response, connectionPlan);
    if (const auto* failure = result.failure()) {
        throw failure->exception();
    }
    return *result.committed();
}

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

[[nodiscard]] inline Http1ServerConnectionPlan finalizeBufferedRouteResponse(
    HttpResponse& response,
    Http1ServerConnectionPlan connectionPlan,
    Http1RequestSequence& requestSequence) {
    connectionPlan =
        requestSequence.completeUncommittedResponse(connectionPlan);
    return requireHttp1FinalResponseCommit(response, connectionPlan);
}

[[nodiscard]] inline Http1ServerConnectionPlan finalizeBodyRouteResponse(
    HttpResponse& response,
    Http1ServerConnectionPlan connectionPlan,
    Http1RequestSequence& requestSequence,
    Http1RequestBodyConsumption bodyConsumption) {
    connectionPlan = http1ApplyRequestBodyConsumption(connectionPlan, bodyConsumption);
    connectionPlan =
        requestSequence.completeUncommittedResponse(connectionPlan);
    // Fix borrowed response views before callers restore pipeline bytes.
    materializeResponseBody(response);
    return requireHttp1FinalResponseCommit(response, connectionPlan);
}

}  // namespace ruvia::detail
