#pragma once

#include "ruvia/web/detail/server/HttpResponseCompression.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/detail/http/HttpCors.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

class HttpBufferedResponsePreparation final {
public:
    // This is the one HTTP-owned snapshot both protocol drivers must consume;
    // neither driver may re-plan after Web compression/CORS has finalized the
    // response representation.
    [[nodiscard]] const HttpBufferedResponseWritePlan& writePlan() const noexcept {
        return writePlan_;
    }

private:
    friend HttpBufferedResponsePreparation prepareBufferedHttpResponse(
        const HttpRequest&,
        HttpContentCoding,
        HttpResponse&,
        const HttpServerOptions&);

    explicit HttpBufferedResponsePreparation(
        HttpBufferedResponseWritePlan writePlan) noexcept
        : writePlan_(writePlan) {}

    HttpBufferedResponseWritePlan writePlan_;
};

[[nodiscard]] inline HttpContentCoding httpResponseCodingFor(const HttpRequest& request) noexcept {
    return httpSelectResponseCoding(requestKnownHeader(request, RequestKnownHeader::kAcceptEncoding));
}

[[nodiscard]] inline HttpBufferedResponsePreparation prepareBufferedHttpResponse(
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
    return HttpBufferedResponsePreparation(
        httpBufferedResponseWritePlan(request.knownMethod(), response));
}

[[nodiscard]] inline HttpBufferedResponsePreparation prepareBufferedHttpResponse(
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
