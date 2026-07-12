#pragma once

#include <memory_resource>
#include <string_view>

#include "ruvia/web/detail/server/HttpResponseCompression.h"
#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/detail/http/HttpCors.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

class HttpBufferedResponsePreparation final {
public:
    // This is the one HTTP-owned snapshot both protocol drivers must consume;
    // neither driver may re-plan after Web compression/CORS has finalized the
    // response representation.
    [[nodiscard]] const HttpBufferedResponseWritePlan& writePlan() const noexcept {
        return writePlan_;
    }

    [[nodiscard]] bool bodyBorrowsCompressionScratch() const noexcept {
        return bodyBorrowsCompressionScratch_;
    }

private:
    friend HttpBufferedResponsePreparation prepareBufferedHttpResponse(
        const HttpRequest&,
        HttpContentCoding,
        HttpResponse&,
        const HttpServerOptions&,
        std::pmr::string&);

    HttpBufferedResponsePreparation(
        HttpBufferedResponseWritePlan writePlan,
        bool bodyBorrowsCompressionScratch) noexcept
        : writePlan_(writePlan),
          bodyBorrowsCompressionScratch_(bodyBorrowsCompressionScratch) {}

    HttpBufferedResponseWritePlan writePlan_;
    bool bodyBorrowsCompressionScratch_{false};
};

[[nodiscard]] inline HttpContentCoding httpResponseCodingFor(const HttpRequest& request) noexcept {
    return httpSelectResponseCoding(requestKnownHeader(request, RequestKnownHeader::kAcceptEncoding));
}

[[nodiscard]] inline HttpBufferedResponsePreparation prepareBufferedHttpResponse(
    const HttpRequest& request,
    HttpContentCoding coding,
    HttpResponse& response,
    const HttpServerOptions& options,
    std::pmr::string& compressionScratch) {
    materializeResponseBody(response);
    applyCorsHeaders(request, response, options.cors);
    const bool bodyBorrowsCompressionScratch = compressResponseBodyIfAccepted(
        coding,
        request.knownMethod(),
        response,
        options.compression,
        compressionScratch);
    return HttpBufferedResponsePreparation(
        httpBufferedResponseWritePlan(request.knownMethod(), response),
        bodyBorrowsCompressionScratch);
}

[[nodiscard]] inline HttpBufferedResponsePreparation prepareBufferedHttpResponse(
    const HttpRequest& request,
    HttpResponse& response,
    const HttpServerOptions& options,
    std::pmr::string& compressionScratch) {
    return prepareBufferedHttpResponse(
        request,
        httpResponseCodingFor(request),
        response,
        options,
        compressionScratch);
}

}  // namespace ruvia::detail
