#pragma once

#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/detail/server/http1/Http1RequestSequence.h"

namespace ruvia::detail {

[[nodiscard]] inline Http1ServerConnectionPlan requireHttp1FinalResponseCommit(HttpResponse& response, Http1ServerConnectionPlan connectionPlan) {
    const auto result = http1CommitFinalResponse(response, connectionPlan);
    if (const auto* failure = result.failure()) {
        throw failure->exception();
    }
    return *result.committed();
}

[[nodiscard]] inline Http1ServerConnectionPlan finalizeBufferedRouteResponse(HttpResponse& response, Http1ServerConnectionPlan connectionPlan, Http1RequestSequence& requestSequence) {
    connectionPlan = requestSequence.completeUncommittedResponse(connectionPlan);
    return requireHttp1FinalResponseCommit(response, connectionPlan);
}

[[nodiscard]] inline Http1ServerConnectionPlan finalizeBodyRouteResponse(HttpResponse& response, Http1ServerConnectionPlan connectionPlan, Http1RequestSequence& requestSequence, Http1RequestBodyConsumption bodyConsumption) {
    connectionPlan = http1ApplyRequestBodyConsumption(connectionPlan, bodyConsumption);
    connectionPlan = requestSequence.completeUncommittedResponse(connectionPlan);
    // Fix borrowed response views before callers restore pipeline bytes.
    materializeResponseBody(response);
    return requireHttp1FinalResponseCommit(response, connectionPlan);
}

}  // namespace ruvia::detail
