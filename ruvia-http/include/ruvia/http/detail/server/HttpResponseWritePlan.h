#pragma once

#include <cstdint>

#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseContentSemantics.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

class HttpResponseBodyPlan final {
public:
    [[nodiscard]] std::uint16_t responseStatus() const noexcept {
        return responseStatus_;
    }

    [[nodiscard]] const ResponseWritePolicy& policy() const noexcept {
        return policy_;
    }

    [[nodiscard]] bool statusAllowsBody() const noexcept {
        return policy_.bodyAllowed();
    }

    [[nodiscard]] bool bodySuppressed() const noexcept {
        return !policy_.bodyAllowed() ||
            semantics_.withContent() == nullptr;
    }

    [[nodiscard]] const HttpResponseContentSemantics&
    contentSemantics() const noexcept {
        return semantics_;
    }

private:
    friend HttpResponseBodyPlan httpResponseBodyPlan(HttpKnownMethod, std::uint16_t) noexcept;
    friend class HttpBufferedResponseWritePlan;

    HttpResponseBodyPlan(
        std::uint16_t responseStatus,
        ResponseWritePolicy policy,
        HttpResponseContentSemantics semantics) noexcept
        : responseStatus_(responseStatus),
          policy_(policy),
          semantics_(semantics) {}

    std::uint16_t responseStatus_;
    ResponseWritePolicy policy_;
    HttpResponseContentSemantics semantics_;
};

[[nodiscard]] inline HttpResponseBodyPlan httpResponseBodyPlan(
    HttpKnownMethod requestMethod,
    std::uint16_t statusCode) noexcept {
    const auto policy = responseWritePolicy(statusCode);
    return HttpResponseBodyPlan(
        statusCode,
        policy,
        httpResponseContentSemantics(requestMethod, statusCode));
}

class HttpBufferedResponseWritePlan final {
public:
    [[nodiscard]] std::uint16_t responseStatus() const noexcept {
        return bodyPlan_.responseStatus();
    }

    [[nodiscard]] const HttpResponseBodyPlan& bodyPlan() const noexcept {
        return bodyPlan_;
    }

    [[nodiscard]] const ResponseWritePolicy& policy() const noexcept {
        return bodyPlan_.policy();
    }

    [[nodiscard]] bool bodySuppressed() const noexcept {
        return bodyPlan_.bodySuppressed();
    }

    [[nodiscard]] bool statusAllowsBody() const noexcept {
        return bodyPlan_.statusAllowsBody();
    }

    [[nodiscard]] std::uint64_t contentLength() const noexcept {
        return contentLength_;
    }

    [[nodiscard]] bool sendBody() const noexcept {
        return !bodySuppressed() && contentLength_ != 0;
    }

private:
    friend HttpBufferedResponseWritePlan httpBufferedResponseWritePlan(
        HttpKnownMethod, const HttpResponse&) noexcept;

    HttpBufferedResponseWritePlan(
        HttpResponseBodyPlan bodyPlan,
        std::uint64_t contentLength) noexcept
        : bodyPlan_(bodyPlan), contentLength_(contentLength) {}

    HttpResponseBodyPlan bodyPlan_;
    std::uint64_t contentLength_{0};
};

[[nodiscard]] inline HttpBufferedResponseWritePlan httpBufferedResponseWritePlan(
    HttpKnownMethod requestMethod,
    const HttpResponse& response) noexcept {
    const auto bodyPlan = httpResponseBodyPlan(
        requestMethod,
        response.status());
    std::uint64_t contentLength = 0;
    if (bodyPlan.statusAllowsBody() &&
        bodyPlan.contentSemantics().connectTunnel() == nullptr) {
        contentLength = static_cast<std::uint64_t>(
            responseBody(response).size());
    }
    return HttpBufferedResponseWritePlan(bodyPlan, contentLength);
}

}  // namespace ruvia::detail
