#pragma once

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpParserInternal.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/HttpResponse.h"

#include <string_view>

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

}  // namespace ruvia::detail
