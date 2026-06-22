#pragma once

#include <memory_resource>
#include <string_view>

#include "HttpResponseCompression.h"
#include "../../http/HeaderAcceptUtils.h"
#include "../../http/HttpResponseBodyAccess.h"
#include "../../http/HttpRequestInternal.h"
#include "../../http/HttpCors.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

struct HttpBufferedResponsePreparation final {
    bool skipBody{false};
    bool bodyBorrowsCompressionScratch{false};
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
    const bool skipBody = request.method() == HttpMethod::kHead;
    const bool bodyBorrowsCompressionScratch = compressResponseBodyIfAccepted(
        coding,
        response,
        options.compression,
        compressionScratch,
        skipBody);
    return HttpBufferedResponsePreparation{
        .skipBody = skipBody,
        .bodyBorrowsCompressionScratch = bodyBorrowsCompressionScratch};
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
