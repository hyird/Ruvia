#pragma once

#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/HttpResponse.h"
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

enum class ResponseStreamKind : std::uint8_t {
    kGeneric,
    kSse
};

class ResponseStreamHead final {
public:
    ResponseStreamHead(HttpResponse response, HttpResponseBodyPlan bodyPlan)
        : response_(std::move(response)),
          bodyPlan_(bodyPlan) {}

    [[nodiscard]] HttpResponse& response() noexcept {
        return response_;
    }

    [[nodiscard]] const ResponseWritePolicy& policy() const noexcept {
        return bodyPlan_.policy();
    }

    [[nodiscard]] bool bodySuppressed() const noexcept {
        return bodyPlan_.bodySuppressed();
    }

private:
    HttpResponse response_;
    HttpResponseBodyPlan bodyPlan_;
};

[[nodiscard]] inline ResponseStreamHead prepareResponseStreamHead(
    HttpResponse response,
    ResponseStreamKind kind,
    ResponseStreamFraming framing,
    HttpResponseBodyPlan bodyPlan,
    bool connectionWillClose = false) {
    const auto& policy = bodyPlan.policy();
    const bool needsSseContentType =
        kind == ResponseStreamKind::kSse &&
        !responseHasKnownHeader(response, kResponseHeaderContentType);
    const bool needsHttp1Chunked =
        framing == ResponseStreamFraming::kHttp1Chunked &&
        policy.transferEncodingAllowed() &&
        !responseHasKnownHeader(response, kResponseHeaderTransferEncoding);
    const bool needsSseCacheControl =
        kind == ResponseStreamKind::kSse &&
        (framing == ResponseStreamFraming::kHttp2DataFrames || policy.transferEncodingAllowed()) &&
        !responseHasKnownHeader(response, kResponseHeaderCacheControl);
    // The stream head is written before the session finalizes the connection
    // lifetime, so the caller passes its keep-alive verdict (connectionWillClose,
    // which already folds in the request-limit flip recordCompletedRequest applies
    // afterward). Announce Connection: close whenever this H1 response will be the
    // connection's last -- mirroring the buffered path's markConnectionCloseIfNeeded
    // -- so a client never reuses a socket the session is about to shut. A close-
    // delimited stream always closes (that close IS its framing). HTTP/2 must never
    // carry a connection-specific header (RFC 9113 8.2.2). The handler's own
    // Connection header, if any, wins.
    const bool needsConnectionClose =
        framing != ResponseStreamFraming::kHttp2DataFrames &&
        (framing == ResponseStreamFraming::kHttp1CloseDelimited || connectionWillClose) &&
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

    return ResponseStreamHead(std::move(response), bodyPlan);
}

}  // namespace ruvia::detail
