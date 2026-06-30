#pragma once

#include "HttpResponseHeadPolicy.h"
#include "../../http/HttpResponseHeaderState.h"
#include "../../http/ContextInternal.h"
#include "ruvia/http/HttpTypes.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace ruvia::detail {

enum class ResponseStreamFraming : std::uint8_t {
    kHttp1Chunked,
    kHttp2DataFrames
};

class ResponseStreamHead final {
public:
    ResponseStreamHead(HttpResponse response, ResponseWritePolicy policy, bool bodyForbidden)
        : response_(std::move(response)),
          policy_(policy),
          bodyForbidden_(bodyForbidden) {}

    [[nodiscard]] HttpResponse& response() noexcept {
        return response_;
    }

    [[nodiscard]] const ResponseWritePolicy& policy() const noexcept {
        return policy_;
    }

    [[nodiscard]] bool bodyForbidden() const noexcept {
        return bodyForbidden_;
    }

private:
    HttpResponse response_;
    ResponseWritePolicy policy_;
    bool bodyForbidden_{false};
};

[[nodiscard]] inline ResponseStreamHead prepareResponseStreamHead(
    Context& context,
    ResponseBodyMode mode,
    ResponseStreamFraming framing) {
    auto response = ContextAccess::streamingHead(context);
    const auto policy = responseWritePolicy(response.statusCode());
    const bool needsSseContentType =
        mode == ResponseBodyMode::kSse &&
        !responseHasKnownHeader(response, kResponseHeaderContentType);
    const bool needsHttp1Chunked =
        framing == ResponseStreamFraming::kHttp1Chunked &&
        policy.transferEncodingAllowed() &&
        !responseHasKnownHeader(response, kResponseHeaderTransferEncoding);
    const bool needsSseCacheControl =
        mode == ResponseBodyMode::kSse &&
        (framing == ResponseStreamFraming::kHttp2DataFrames || policy.transferEncodingAllowed()) &&
        !responseHasKnownHeader(response, kResponseHeaderCacheControl);

    const auto additionalHeaders =
        static_cast<std::size_t>(needsSseContentType) +
        static_cast<std::size_t>(needsHttp1Chunked) +
        static_cast<std::size_t>(needsSseCacheControl);
    if (additionalHeaders != 0) {
        reserveResponseHeaders(response, response.headers().size() + additionalHeaders);
    }

    if (needsSseContentType) {
        setResponseHeaderStableView(response, "Content-Type", "text/event-stream");
    }
    if (framing == ResponseStreamFraming::kHttp1Chunked && policy.transferEncodingAllowed()) {
        setResponseHeaderStableView(response, "Transfer-Encoding", "chunked");
    }
    if (mode == ResponseBodyMode::kSse &&
        (framing == ResponseStreamFraming::kHttp2DataFrames || policy.transferEncodingAllowed())) {
        setResponseHeaderStableView(response, "Cache-Control", "no-store");
    }

    return ResponseStreamHead(std::move(response), policy, !policy.bodyAllowed());
}

}  // namespace ruvia::detail
