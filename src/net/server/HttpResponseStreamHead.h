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
    // RFC 9112 6.1: a server MUST NOT send Transfer-Encoding to a client that did
    // not indicate HTTP/1.1. An HTTP/1.0 stream therefore carries no chunk framing;
    // the body is delimited by the connection close (RFC 9112 6.3), so it also
    // announces Connection: close and the session shuts the socket afterwards.
    kHttp1CloseDelimited,
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
    const auto policy = responseWritePolicy(response.status());
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
    // A close-delimited HTTP/1.0 stream has no chunk framing and no Content-Length,
    // so the connection close is the message boundary: advertise Connection: close
    // (also overriding an HTTP/1.0 "Connection: keep-alive" request) unless the
    // handler already set a Connection header.
    const bool needsConnectionClose =
        framing == ResponseStreamFraming::kHttp1CloseDelimited &&
        !responseHasKnownHeader(response, kResponseHeaderConnection);

    const auto additionalHeaders =
        static_cast<std::size_t>(needsSseContentType) +
        static_cast<std::size_t>(needsHttp1Chunked) +
        static_cast<std::size_t>(needsSseCacheControl) +
        static_cast<std::size_t>(needsConnectionClose);
    if (additionalHeaders != 0) {
        reserveResponseHeaders(response, response.headers().size() + additionalHeaders);
    }

    if (needsSseContentType) {
        setResponseHeaderStableView(response, "Content-Type", "text/event-stream");
    }
    if (framing == ResponseStreamFraming::kHttp1Chunked && policy.transferEncodingAllowed()) {
        setResponseHeaderStableView(response, "Transfer-Encoding", "chunked");
    }
    if (needsConnectionClose) {
        setResponseHeaderStableView(response, "Connection", "close");
    }
    if (needsSseCacheControl) {
        // Gate on the guard that was already computed for the reserve count above,
        // which includes !responseHasKnownHeader(...Cache-Control). Re-inlining only
        // the mode/framing condition here (as before) dropped that guard and
        // overwrote a handler's own Cache-Control -- e.g. the recommended SSE
        // "no-cache" -- with "no-store". This mirrors the Content-Type path, which
        // uses needsSseContentType, so a caller-provided value is honored.
        setResponseHeaderStableView(response, "Cache-Control", "no-store");
    }

    return ResponseStreamHead(std::move(response), policy, !policy.bodyAllowed());
}

}  // namespace ruvia::detail
