#pragma once

// Stable, non-detail entry points for HTTP response serialization plans. The
// plan implementations remain shared with the HTTP/1 and HTTP/2 protocol code.
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/Http1ResponseHeadPlan.h"
#include "ruvia/http/HttpResponseHeadBuffer.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"

#include <memory_resource>
#include <span>
#include <string>
#include <utility>

namespace ruvia {

// Serialize a finalized HTTP/1 response head into caller-owned reusable scratch.
// The plan must describe the same response representation.
void appendHttp1ResponseHead(
    const HttpResponse& response, HttpResponseHeadBuffer& head,
    const Http1ResponseHeadPlan& plan);

void appendHttp1ResponseTrailers(
    std::pmr::string& output, const detail::HttpResponseTrailerSection& trailers);

using HttpServerResponseBodyPlan = detail::HttpResponseBodyPlan;
using HttpServerBufferedResponseWritePlan = detail::HttpBufferedResponseWritePlan;
using ResponseStreamFraming = detail::ResponseStreamFraming;
using ResponseStreamKind = detail::ResponseStreamKind;
using ResponseTrailerIntent = detail::ResponseTrailerIntent;
using ResponseStreamTrailerFraming = detail::ResponseStreamTrailerFraming;
using ResponseStreamHeadDisposition = detail::ResponseStreamHeadDisposition;
using ResponseStreamCommitPlan = detail::ResponseStreamCommitPlan;
using ResponseStreamHead = detail::ResponseStreamHead;
using HttpResponseTrailerSection = detail::HttpResponseTrailerSection;

// Validate a complete response trailer section and return a borrowed proof for
// synchronous protocol submission. The input storage must outlive its use.
[[nodiscard]] inline HttpResponseTrailerSection validateHttpResponseTrailers(
    std::span<const HttpHeaderView> trailers) {
    auto result = detail::validatedResponseTrailerSection(trailers);
    return *result.section();
}

[[nodiscard]] inline HttpServerResponseBodyPlan planHttpServerResponseBody(
    HttpKnownMethod requestMethod, HttpStatusCode status) noexcept {
    return detail::httpResponseBodyPlan(requestMethod, status);
}

[[nodiscard]] inline HttpServerBufferedResponseWritePlan planHttpServerBufferedResponseWrite(
    HttpKnownMethod requestMethod, const HttpResponse& response) noexcept {
    return detail::httpBufferedResponseWritePlan(requestMethod, response);
}

[[nodiscard]] inline ResponseStreamCommitPlan planHttpResponseStreamCommit(
    ResponseStreamFraming framing, HttpKnownMethod requestMethod, HttpStatusCode status,
    ResponseTrailerIntent trailerIntent) noexcept {
    return detail::httpResponseStreamCommitPlan(framing, requestMethod, status, trailerIntent);
}

[[nodiscard]] inline ResponseStreamHead prepareHttpResponseStreamHead(
    HttpResponse response, ResponseStreamKind kind, ResponseStreamCommitPlan commitPlan) {
    return detail::prepareResponseStreamHead(std::move(response), kind, commitPlan);
}

[[nodiscard]] inline ResponseTrailerIntent httpResponseTrailerIntent(
    const HttpResponseTrailerSection& section) noexcept {
    return detail::responseTrailerIntent(section);
}

}  // namespace ruvia
