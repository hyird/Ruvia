#pragma once

#include <memory_resource>
#include <string_view>

#include "HttpResponseCompression.h"
#include "../../http/HttpCors.h"
#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpParser.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

struct HttpBufferedResponsePreparation final {
    bool skipBody{false};
    bool borrowedCompressionScratch{false};
};

[[nodiscard]] inline HttpRequestFlags httpResponseCompressionFlags(std::string_view acceptEncoding) noexcept {
    HttpRequestFlags flags;
    int explicitQuality = -1;
    int wildcardQuality = -1;
    httpUpdateAcceptedEncodingQuality(
        acceptEncoding,
        "gzip",
        explicitQuality,
        wildcardQuality);
    flags.acceptsGzip = explicitQuality >= 0 ? explicitQuality > 0 : wildcardQuality > 0;
    return flags;
}

[[nodiscard]] inline HttpRequestFlags httpResponseCompressionFlags(const HttpRequest& request) noexcept {
    return httpResponseCompressionFlags(request.header(HttpRequest::KnownHeader::kAcceptEncoding));
}

[[nodiscard]] inline HttpBufferedResponsePreparation prepareBufferedHttpResponse(
    const HttpRequest& request,
    const HttpRequestFlags& requestFlags,
    HttpResponse& response,
    const HttpServerOptions& options,
    std::pmr::string* compressionScratch) {
    response.materializeBody();
    applyCorsHeaders(request, response, options.cors);
    const bool skipBody = request.method() == HttpMethod::kHead;
    const bool borrowedCompressionScratch = compressResponseBodyIfAccepted(
        requestFlags,
        response,
        options.compression,
        compressionScratch,
        skipBody);
    return HttpBufferedResponsePreparation{
        .skipBody = skipBody,
        .borrowedCompressionScratch = borrowedCompressionScratch};
}

[[nodiscard]] inline HttpBufferedResponsePreparation prepareBufferedHttpResponse(
    const HttpRequest& request,
    HttpResponse& response,
    const HttpServerOptions& options,
    std::pmr::string* compressionScratch) {
    return prepareBufferedHttpResponse(
        request,
        httpResponseCompressionFlags(request),
        response,
        options,
        compressionScratch);
}

}  // namespace ruvia::detail
