#pragma once

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpParserInternal.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/HttpResponse.h"

#include <string_view>
#include <utility>

namespace ruvia::detail {

inline constexpr std::string_view kHttp1ContinueResponse =
    "HTTP/1.1 100 Continue\r\n\r\n";

[[nodiscard]] inline bool http1ShouldKeepAlive(const HttpServerParseResult& parsed) noexcept {
    if (parsed.flags.connectionClose) {
        return false;
    }
    if (parsed.flags.connectionKeepAlive) {
        return true;
    }
    return parsed.request.httpVersion() == "HTTP/1.1";
}

class Http1ResponseStreamPlan final {
public:
    [[nodiscard]] ResponseStreamFraming framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] bool requestCanPersist() const noexcept {
        return requestCanPersist_;
    }

    [[nodiscard]] bool connectionWillClose() const noexcept {
        return connectionWillClose_;
    }

    [[nodiscard]] bool needsKeepAliveSignal() const noexcept {
        return needsKeepAliveSignal_;
    }

    [[nodiscard]] HttpMethod requestMethod() const noexcept {
        return requestMethod_;
    }

private:
    friend Http1ResponseStreamPlan http1PlanResponseStream(
        const HttpServerParseResult&, bool) noexcept;

    Http1ResponseStreamPlan(
        ResponseStreamFraming framing,
        bool requestCanPersist,
        bool connectionWillClose,
        bool needsKeepAliveSignal,
        HttpMethod requestMethod) noexcept
        : framing_(framing),
          requestCanPersist_(requestCanPersist),
          connectionWillClose_(connectionWillClose),
          needsKeepAliveSignal_(needsKeepAliveSignal),
          requestMethod_(requestMethod) {}

    ResponseStreamFraming framing_;
    bool requestCanPersist_{false};
    bool connectionWillClose_{true};
    bool needsKeepAliveSignal_{false};
    HttpMethod requestMethod_{HttpMethod::kUnknown};
};

// Pure HTTP/1 response-stream planning. The runtime contributes only its product
// policy bit (for example, a per-connection request limit); HTTP owns every
// version/body/framing/persistence decision and returns a combination that cannot
// represent HTTP/1.0 chunked framing or a reusable close-delimited connection.
[[nodiscard]] inline Http1ResponseStreamPlan http1PlanResponseStream(
    const HttpServerParseResult& parsed,
    bool closeForServerPolicy) noexcept {
    const bool isHttp11 = parsed.request.httpVersion() == "HTTP/1.1";
    const bool requestCanPersist =
        http1ShouldKeepAlive(parsed) && parsed.contentLength == 0 && !parsed.chunked;
    const auto framing = isHttp11
        ? ResponseStreamFraming::kHttp1Chunked
        : ResponseStreamFraming::kHttp1CloseDelimited;
    const bool connectionWillClose =
        !requestCanPersist || !isHttp11 || closeForServerPolicy;
    return Http1ResponseStreamPlan(
        framing,
        requestCanPersist,
        connectionWillClose,
        !isHttp11,
        parsed.request.method());
}

[[nodiscard]] inline bool http1WantsContinue(const HttpServerParseResult& parsed) noexcept {
    // Only HTTP/1.1 clients understand interim 1xx responses. An HTTP/1.0 client
    // would read "100 Continue" as the final response, so its Expect flag is ignored.
    return parsed.flags.expectContinue && parsed.request.httpVersion() == "HTTP/1.1";
}

[[nodiscard]] inline bool http1ResponseWantsClose(const HttpResponse& response) noexcept {
    return httpHasToken(responseKnownHeader(response, kResponseHeaderConnection), "close");
}

inline void http1MarkConnectionClose(HttpResponse& response) {
    setResponseHeaderStableView(response, "Connection", "close");
}

inline void http1MarkConnectionCloseIfNeeded(HttpResponse& response, bool keepAlive) {
    if (!keepAlive) {
        http1MarkConnectionClose(response);
    }
}

// RFC 9112 section 9.3: HTTP/1.0 defaults to closing the connection, so a server
// that keeps it open must advertise the keep-alive connection option. HTTP/1.1 is
// persistent by default and needs no such response header.
[[nodiscard]] inline bool http1RequestNeedsKeepAliveSignal(std::string_view httpVersion) noexcept {
    return httpVersion != "HTTP/1.1";
}

inline void http1MarkConnectionKeepAliveIfNeeded(
    HttpResponse& response,
    bool keepAlive,
    bool needsKeepAliveSignal) {
    if (keepAlive && needsKeepAliveSignal &&
        !responseHasKnownHeader(response, kResponseHeaderConnection)) {
        setResponseHeaderStableView(response, "Connection", "keep-alive");
    }
}

// Finalize response-side HTTP/1 persistence after the runtime has folded in
// request-body completion and server policy. This is the sole protocol mutation:
// it honors an application-provided Connection: close and emits the version-
// appropriate response signal. The returned value is the authoritative reuse verdict.
[[nodiscard]] inline bool http1FinalizeResponseConnection(
    HttpResponse& response,
    bool keepAlive,
    bool needsKeepAliveSignal) {
    if (http1ResponseWantsClose(response)) {
        keepAlive = false;
    }
    http1MarkConnectionCloseIfNeeded(response, keepAlive);
    http1MarkConnectionKeepAliveIfNeeded(response, keepAlive, needsKeepAliveSignal);
    return keepAlive;
}

[[nodiscard]] inline ResponseStreamHead prepareHttp1ResponseStreamHead(
    HttpResponse response,
    ResponseStreamKind kind,
    const Http1ResponseStreamPlan& plan) {
    const auto bodyPlan = httpResponseBodyPlan(plan.requestMethod(), response.status());
    return prepareResponseStreamHead(
        std::move(response),
        kind,
        plan.framing(),
        bodyPlan,
        plan.connectionWillClose());
}

}  // namespace ruvia::detail
