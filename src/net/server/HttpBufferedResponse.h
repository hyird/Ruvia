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

[[nodiscard]] inline bool httpRequestAcceptsGzip(std::string_view acceptEncoding) noexcept {
    return httpAcceptsEncoding(acceptEncoding, "gzip");
}

[[nodiscard]] inline bool httpRequestAcceptsGzip(const HttpRequest& request) noexcept {
    return httpRequestAcceptsGzip(requestKnownHeader(request, RequestKnownHeader::kAcceptEncoding));
}

[[nodiscard]] inline HttpBufferedResponsePreparation prepareBufferedHttpResponse(
    const HttpRequest& request,
    bool acceptsGzip,
    HttpResponse& response,
    const HttpServerOptions& options,
    std::pmr::string& compressionScratch) {
    materializeResponseBody(response);
    applyCorsHeaders(request, response, options.cors);
    const bool skipBody = request.method() == HttpMethod::kHead;
    const bool bodyBorrowsCompressionScratch = compressResponseBodyIfAccepted(
        acceptsGzip,
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
        httpRequestAcceptsGzip(request),
        response,
        options,
        compressionScratch);
}

}  // namespace ruvia::detail
