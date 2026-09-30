#pragma once

#include "ruvia/http/Http1ServerSemantics.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/detail/server/http1/Http1RequestSequence.h"

namespace ruvia::detail {

[[nodiscard]] inline Http1RequestConnectionPlan requireHttp1FinalResponseCommit(
    HttpResponse& response, Http1RequestConnectionPlan connectionPlan) {
    const auto result = http1CommitFinalResponse(response, connectionPlan);
    if (const auto* failure = result.failure()) {
        throw failure->exception();
    }
    return *result.committed();
}

[[nodiscard]] inline Http1RequestConnectionPlan finalizeBufferedRouteResponse(HttpResponse& response,
    Http1RequestConnectionPlan connectionPlan, Http1RequestSequence& requestSequence) {
    connectionPlan = requestSequence.completeUncommittedResponse(connectionPlan);
    return requireHttp1FinalResponseCommit(response, connectionPlan);
}

[[nodiscard]] inline Http1RequestConnectionPlan finalizeBodyRouteResponse(HttpResponse& response,
    Http1RequestConnectionPlan connectionPlan, Http1RequestSequence& requestSequence,
    Http1RequestBodyConsumption bodyConsumption) {
    connectionPlan = applyRequestBodyConsumption(connectionPlan, bodyConsumption);
    connectionPlan = requestSequence.completeUncommittedResponse(connectionPlan);
    // Fix borrowed response views before callers restore pipeline bytes.
    response.materializeBody();
    return requireHttp1FinalResponseCommit(response, connectionPlan);
}

}  // namespace ruvia::detail
