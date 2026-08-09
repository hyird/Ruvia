#pragma once

#include "ruvia/http/Http1ClosePolicy.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"

#include <cstdint>

namespace ruvia::detail {

// Immutable server-side connection contract. The exact request protocol version
// and disposition are deliberately inseparable: reducing the former to a
// keep-alive signal loses the version needed by the response status-line and
// final control validation. Transformations can only tighten the plan to kClose.
class Http1ServerConnectionPlan final {
public:
    // Errors produced before a valid request version exists use the server's
    // native HTTP/1.1 response version and always close. Once parsing succeeds,
    // callers must preserve the parsed plan and use requireClose().
    [[nodiscard]] static constexpr Http1ServerConnectionPlan http11Close() noexcept {
        return Http1ServerConnectionPlan(HttpProtocolVersion::kHttp11, Http1ClosePolicy::kCloseAfterResponse);
    }

    [[nodiscard]] constexpr HttpProtocolVersion protocolVersion() const noexcept {
        return protocolVersion_;
    }

    [[nodiscard]] constexpr Http1ClosePolicy disposition() const noexcept {
        return disposition_;
    }

    [[nodiscard]] constexpr Http1ServerConnectionPlan requireClose() const noexcept {
        return Http1ServerConnectionPlan(protocolVersion_, Http1ClosePolicy::kCloseAfterResponse);
    }

private:
    friend Http1ServerConnectionPlan http1PlanHttp10RequestConnection(const HttpConnectionOptions&) noexcept;
    friend Http1ServerConnectionPlan http1PlanHttp11RequestConnection(const HttpConnectionOptions&) noexcept;

    constexpr Http1ServerConnectionPlan(HttpProtocolVersion protocolVersion, Http1ClosePolicy disposition) noexcept
        : protocolVersion_(protocolVersion),
          disposition_(disposition) {}

    HttpProtocolVersion protocolVersion_;
    Http1ClosePolicy disposition_;
};

[[nodiscard]] inline Http1ServerConnectionPlan http1PlanHttp10RequestConnection(const HttpConnectionOptions& options) noexcept {
    return Http1ServerConnectionPlan(HttpProtocolVersion::kHttp10, !options.close() && options.keepAlive() ? Http1ClosePolicy::kAllowReuse : Http1ClosePolicy::kCloseAfterResponse);
}

[[nodiscard]] inline Http1ServerConnectionPlan http1PlanHttp11RequestConnection(const HttpConnectionOptions& options) noexcept {
    return Http1ServerConnectionPlan(HttpProtocolVersion::kHttp11, options.close() ? Http1ClosePolicy::kCloseAfterResponse : Http1ClosePolicy::kAllowReuse);
}

}  // namespace ruvia::detail
