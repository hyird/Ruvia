#pragma once

#include <cstdint>
#include <type_traits>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/coding/HttpResponseContentSemantics.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"

namespace ruvia {

class HttpServerResponseBodyPlan final {
public:
    [[nodiscard]] HttpKnownMethod requestMethod() const noexcept { return requestMethod_; }
    [[nodiscard]] HttpStatusCode responseStatus() const noexcept { return responseStatus_; }
    [[nodiscard]] detail::ResponseWritePolicy policy() const noexcept { return policy_; }
    [[nodiscard]] bool statusAllowsBody() const noexcept { return policy_.bodyAllowed(); }
    [[nodiscard]] bool bodySuppressed() const noexcept {
        return !policy_.bodyAllowed() || semantics_ != detail::HttpResponseContentSemantics::kWithContent;
    }
    [[nodiscard]] detail::HttpResponseContentSemantics contentSemantics() const noexcept { return semantics_; }
    [[nodiscard]] std::uint64_t bufferedRepresentationLength(const HttpResponse& response) const noexcept {
        if (!statusAllowsBody() || semantics_ == detail::HttpResponseContentSemantics::kConnectTunnel) {
            return 0;
        }
        return static_cast<std::uint64_t>(detail::responseBody(response).size());
    }

private:
    friend HttpServerResponseBodyPlan planHttpServerResponseBody(
        HttpKnownMethod, HttpStatusCode) noexcept;
    friend class HttpServerBufferedResponseWritePlan;
    constexpr HttpServerResponseBodyPlan(HttpKnownMethod requestMethod, HttpStatusCode responseStatus,
        detail::ResponseWritePolicy policy, detail::HttpResponseContentSemantics semantics) noexcept
        : requestMethod_(requestMethod), responseStatus_(responseStatus), policy_(policy), semantics_(semantics) {}
    HttpKnownMethod requestMethod_;
    HttpStatusCode responseStatus_;
    detail::ResponseWritePolicy policy_;
    detail::HttpResponseContentSemantics semantics_;
};

static_assert(std::is_trivially_copyable_v<HttpServerResponseBodyPlan>);
static_assert(sizeof(HttpServerResponseBodyPlan) <= 12);

class HttpServerBufferedResponseWritePlan final {
public:
    [[nodiscard]] HttpKnownMethod requestMethod() const noexcept { return bodyPlan_.requestMethod(); }
    [[nodiscard]] HttpStatusCode responseStatus() const noexcept { return bodyPlan_.responseStatus(); }
    [[nodiscard]] HttpServerResponseBodyPlan bodyPlan() const noexcept { return bodyPlan_; }
    [[nodiscard]] detail::ResponseWritePolicy policy() const noexcept { return bodyPlan_.policy(); }
    [[nodiscard]] bool bodySuppressed() const noexcept { return bodyPlan_.bodySuppressed(); }
    [[nodiscard]] bool statusAllowsBody() const noexcept { return bodyPlan_.statusAllowsBody(); }
    [[nodiscard]] std::uint64_t contentLength() const noexcept { return contentLength_; }
    [[nodiscard]] bool sendBody() const noexcept { return !bodySuppressed() && contentLength_ != 0; }
    [[nodiscard]] bool matchesResponse(const HttpResponse& response) const noexcept {
        return responseStatus() == response.status() &&
               contentLength_ == bodyPlan_.bufferedRepresentationLength(response);
    }

private:
    friend HttpServerBufferedResponseWritePlan planHttpServerBufferedResponseWrite(
        HttpKnownMethod, const HttpResponse&) noexcept;
    HttpServerBufferedResponseWritePlan(HttpServerResponseBodyPlan bodyPlan, std::uint64_t contentLength) noexcept
        : bodyPlan_(bodyPlan), contentLength_(contentLength) {}
    HttpServerResponseBodyPlan bodyPlan_;
    std::uint64_t contentLength_{0};
};

[[nodiscard]] inline HttpServerResponseBodyPlan planHttpServerResponseBody(
    HttpKnownMethod requestMethod, HttpStatusCode statusCode) noexcept {
    return HttpServerResponseBodyPlan(requestMethod, statusCode, detail::responseWritePolicy(statusCode),
        detail::httpResponseContentSemantics(requestMethod, statusCode));
}

[[nodiscard]] inline HttpServerBufferedResponseWritePlan planHttpServerBufferedResponseWrite(
    HttpKnownMethod requestMethod, const HttpResponse& response) noexcept {
    const auto bodyPlan = planHttpServerResponseBody(requestMethod, response.status());
    return HttpServerBufferedResponseWritePlan(bodyPlan, bodyPlan.bufferedRepresentationLength(response));
}

}  // namespace ruvia

namespace ruvia::detail {
using HttpResponseBodyPlan = ::ruvia::HttpServerResponseBodyPlan;
using HttpBufferedResponseWritePlan = ::ruvia::HttpServerBufferedResponseWritePlan;
[[nodiscard]] inline HttpResponseBodyPlan httpResponseBodyPlan(
    HttpKnownMethod method, HttpStatusCode status) noexcept {
    return ::ruvia::planHttpServerResponseBody(method, status);
}
[[nodiscard]] inline HttpBufferedResponseWritePlan httpBufferedResponseWritePlan(
    HttpKnownMethod method, const HttpResponse& response) noexcept {
    return ::ruvia::planHttpServerBufferedResponseWrite(method, response);
}
}  // namespace ruvia::detail
