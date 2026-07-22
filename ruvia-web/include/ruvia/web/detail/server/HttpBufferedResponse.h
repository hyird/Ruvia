#pragma once

#include "ruvia/web/detail/server/HttpResponseCompression.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/http/detail/HttpAcceptEncoding.h"
#include "ruvia/http/detail/HttpContentCoding.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/detail/http/HttpCors.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

[[nodiscard]] inline HttpContentCoding httpResponseCodingFor(const HttpRequest& request) noexcept {
    HttpResponseCodingQualities qualities;
    for (const auto& header : request.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Accept-Encoding")) {
            qualities.update(header.value());
        }
    }
    return httpSelectResponseCodingFromQualities(qualities);
}

// This returns the one HTTP-owned snapshot both protocol drivers must consume;
// neither driver may re-plan after Web compression/CORS has finalized the
// response representation.
[[nodiscard]] inline HttpBufferedResponseWritePlan prepareBufferedHttpResponse(
    const HttpRequest& request,
    HttpContentCoding coding,
    HttpResponse& response,
    const HttpServerOptions& options) {
    materializeResponseBody(response);
    if (options.cors.has_value()) {
        applyCorsHeaders(request, response, *options.cors);
    }
    if (options.compression.has_value()) {
        applyResponseCompression(
            coding,
            request.knownMethod(),
            response,
            *options.compression);
    }
    return httpBufferedResponseWritePlan(request.knownMethod(), response);
}

[[nodiscard]] inline HttpBufferedResponseWritePlan prepareBufferedHttpResponse(
    const HttpRequest& request,
    HttpResponse& response,
    const HttpServerOptions& options) {
    return prepareBufferedHttpResponse(
        request,
        httpResponseCodingFor(request),
        response,
        options);
}

}  // namespace ruvia::detail
