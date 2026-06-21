#pragma once

#include "HttpResponseWriter.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpTypes.h"

#include <cstdint>
#include <string_view>
#include <utility>

namespace ruvia::detail {

enum class ResponseStreamFraming : std::uint8_t {
    kHttp1Chunked,
    kHttp2DataFrames
};

struct ResponseStreamHead final {
    HttpResponse response;
    ResponseWritePolicy policy;
    bool bodyForbidden{false};
};

[[nodiscard]] inline ResponseStreamHead prepareResponseStreamHead(
    Context& context,
    ResponseBodyMode mode,
    ResponseStreamFraming framing) {
    auto response = context.streamingHead();
    const auto policy = responseWritePolicy(response.statusCode());

    if (mode == ResponseBodyMode::kSse &&
        !response.hasKnownHeader(HttpResponse::kKnownHeaderContentType)) {
        setResponseHeaderStableView(response, "Content-Type", "text/event-stream");
    }
    if (framing == ResponseStreamFraming::kHttp1Chunked && policy.transferEncodingAllowed) {
        setResponseHeaderStableView(response, "Transfer-Encoding", "chunked");
    }
    if (mode == ResponseBodyMode::kSse &&
        (framing == ResponseStreamFraming::kHttp2DataFrames || policy.transferEncodingAllowed)) {
        setResponseHeaderStableView(response, "Cache-Control", "no-store");
    }

    return ResponseStreamHead{
        .response = std::move(response),
        .policy = policy,
        .bodyForbidden = !policy.bodyAllowed};
}

}  // namespace ruvia::detail
