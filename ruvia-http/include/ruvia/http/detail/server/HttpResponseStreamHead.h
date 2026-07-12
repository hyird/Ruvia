#pragma once

#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
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
    kHttp2Frames
};

enum class ResponseStreamKind : std::uint8_t {
    kGeneric,
    kSse
};

// A trailer section is terminal message metadata, not an independently queued
// side channel. The caller declares whether this head commit is reserving a
// terminal trailer section so HTTP/2 can keep a content-forbidden response open
// for trailing HEADERS while HTTP/1 rejects an unavailable representation before
// emitting the response head.
enum class ResponseTrailerIntent : std::uint8_t {
    kNone,
    kPresent
};

enum class ResponseStreamTrailerFraming : std::uint8_t {
    kUnavailable,
    kHttp1Chunked,
    kHttp2TrailingHeaders
};

// Authoritative phase immediately after the initial response head is submitted.
// kTrailersOnly is intentionally distinct from kBodyOpen: HTTP/2 may carry a
// trailer section after a HEAD/204-style response without allowing any DATA.
enum class ResponseStreamHeadDisposition : std::uint8_t {
    kBodyOpen,
    kTrailersOnly,
    kMessageEnded
};

class ResponseStreamCommitPlan final {
public:
    [[nodiscard]] std::uint16_t responseStatus() const noexcept {
        return bodyPlan_.responseStatus();
    }

    [[nodiscard]] ResponseStreamFraming framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] const HttpResponseBodyPlan& bodyPlan() const noexcept {
        return bodyPlan_;
    }

    [[nodiscard]] ResponseStreamTrailerFraming trailerFraming() const noexcept {
        return trailerFraming_;
    }

    [[nodiscard]] ResponseStreamHeadDisposition headDisposition() const noexcept {
        return headDisposition_;
    }

private:
    friend ResponseStreamCommitPlan httpResponseStreamCommitPlan(
        ResponseStreamFraming,
        HttpKnownMethod,
        std::uint16_t,
        ResponseTrailerIntent) noexcept;

    ResponseStreamCommitPlan(
        ResponseStreamFraming framing,
        HttpResponseBodyPlan bodyPlan,
        ResponseStreamTrailerFraming trailerFraming,
        ResponseStreamHeadDisposition headDisposition) noexcept
        : framing_(framing),
          bodyPlan_(bodyPlan),
          trailerFraming_(trailerFraming),
          headDisposition_(headDisposition) {}

    ResponseStreamFraming framing_{ResponseStreamFraming::kHttp2Frames};
    HttpResponseBodyPlan bodyPlan_;
    ResponseStreamTrailerFraming trailerFraming_{ResponseStreamTrailerFraming::kUnavailable};
    ResponseStreamHeadDisposition headDisposition_{ResponseStreamHeadDisposition::kMessageEnded};
};

[[nodiscard]] inline ResponseStreamCommitPlan httpResponseStreamCommitPlan(
    ResponseStreamFraming framing,
    HttpKnownMethod requestMethod,
    std::uint16_t responseStatus,
    ResponseTrailerIntent trailerIntent) noexcept {
    const auto bodyPlan = httpResponseBodyPlan(requestMethod, responseStatus);
    if (framing == ResponseStreamFraming::kHttp2Frames) {
        return ResponseStreamCommitPlan(
            framing,
            bodyPlan,
            ResponseStreamTrailerFraming::kHttp2TrailingHeaders,
            bodyPlan.bodySuppressed()
                ? (trailerIntent == ResponseTrailerIntent::kPresent
                      ? ResponseStreamHeadDisposition::kTrailersOnly
                      : ResponseStreamHeadDisposition::kMessageEnded)
                : ResponseStreamHeadDisposition::kBodyOpen);
    }

    return ResponseStreamCommitPlan(
        framing,
        bodyPlan,
        framing == ResponseStreamFraming::kHttp1Chunked && !bodyPlan.bodySuppressed()
            ? ResponseStreamTrailerFraming::kHttp1Chunked
            : ResponseStreamTrailerFraming::kUnavailable,
        bodyPlan.bodySuppressed()
            ? ResponseStreamHeadDisposition::kMessageEnded
            : ResponseStreamHeadDisposition::kBodyOpen);
}

class ResponseStreamHead final {
public:
    ResponseStreamHead(HttpResponse response, ResponseStreamCommitPlan commitPlan)
        : response_(std::move(response)),
          commitPlan_(std::move(commitPlan)) {}

    [[nodiscard]] HttpResponse& response() noexcept {
        return response_;
    }

    [[nodiscard]] const HttpResponse& response() const noexcept {
        return response_;
    }

    [[nodiscard]] const ResponseStreamCommitPlan& commitPlan() const noexcept {
        return commitPlan_;
    }

private:
    HttpResponse response_;
    ResponseStreamCommitPlan commitPlan_;
};

[[nodiscard]] inline ResponseStreamHead prepareResponseStreamHead(
    HttpResponse response,
    ResponseStreamKind kind,
    ResponseStreamCommitPlan commitPlan) {
    if (response.status() != commitPlan.responseStatus()) {
        throw std::invalid_argument(
            "response stream commit plan status does not match response");
    }
    const auto framing = commitPlan.framing();
    const auto& bodyPlan = commitPlan.bodyPlan();
    const auto& policy = bodyPlan.policy();
    const bool writerOwnsHttp1Chunked =
        framing == ResponseStreamFraming::kHttp1Chunked &&
        policy.transferEncodingAllowed();

    // Keep the prepared response metadata consistent with the wire plan. The
    // framework's chunk writer is the only Transfer-Encoding producer; an
    // HTTP/1.0 close-delimited body cannot retain either framing field. HEAD/304
    // may retain Content-Length metadata because their body is suppressed.
    if (writerOwnsHttp1Chunked) {
        response.header("Content-Length", std::nullopt);
    } else if (framing == ResponseStreamFraming::kHttp1Chunked ||
               framing == ResponseStreamFraming::kHttp1CloseDelimited) {
        response.header("Transfer-Encoding", std::nullopt);
    }
    if (framing == ResponseStreamFraming::kHttp1CloseDelimited &&
        !bodyPlan.bodySuppressed()) {
        response.header("Content-Length", std::nullopt);
    }

    const bool needsSseContentType =
        kind == ResponseStreamKind::kSse &&
        !responseHasKnownHeader(response, kResponseHeaderContentType);
    const bool needsHttp1Chunked =
        writerOwnsHttp1Chunked &&
        !responseHasKnownHeader(response, kResponseHeaderTransferEncoding);
    const bool needsSseCacheControl =
        kind == ResponseStreamKind::kSse &&
        (framing == ResponseStreamFraming::kHttp2Frames || policy.transferEncodingAllowed()) &&
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
    if (writerOwnsHttp1Chunked) {
        setResponseHeaderStableView(response, "Transfer-Encoding", "chunked");
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

    return ResponseStreamHead(std::move(response), std::move(commitPlan));
}

}  // namespace ruvia::detail
