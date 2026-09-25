#pragma once

// Stable, non-detail entry points for HTTP response serialization plans. The
// plan implementations remain shared with the HTTP/1 and HTTP/2 protocol code.
#include <memory_resource>
#include <span>
#include <string>
#include <utility>

#include "ruvia/http/Http1ResponseHeadPlan.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpResponseHeadBuffer.h"
#include "ruvia/http/HttpResponseTrailerSection.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"

namespace ruvia {

// Serialize a finalized HTTP/1 response head into caller-owned reusable scratch.
// The plan must describe the same response representation.
void appendHttp1ResponseHead(
    const HttpResponse& response, HttpResponseHeadBuffer& head,
    const Http1ResponseHeadPlan& plan);

void appendHttp1ResponseTrailers(
    std::pmr::string& output, const HttpResponseTrailerSection& trailers);

// Validate a complete response trailer section and return a borrowed proof for
// synchronous protocol submission. The input storage must outlive its use.
[[nodiscard]] inline HttpResponseTrailerSection validateHttpResponseTrailers(
    std::span<const HttpHeaderView> trailers) {
    auto result = detail::validatedResponseTrailerSection(trailers);
    return *result.section();
}

[[nodiscard]] inline ResponseStreamCommitPlan planHttpResponseStreamCommit(
    ResponseStreamFraming framing, HttpKnownMethod requestMethod, HttpStatusCode status,
    ResponseTrailerIntent trailerIntent) noexcept {
    return httpResponseStreamCommitPlan(framing, requestMethod, status, trailerIntent);
}

[[nodiscard]] inline ResponseStreamHead prepareHttpResponseStreamHead(
    HttpResponse response, ResponseStreamKind kind, ResponseStreamCommitPlan commitPlan) {
    return prepareResponseStreamHead(std::move(response), kind, commitPlan);
}

[[nodiscard]] inline ResponseTrailerIntent httpResponseTrailerIntent(
    const HttpResponseTrailerSection& section) noexcept {
    return responseTrailerIntent(section);
}

}  // namespace ruvia
