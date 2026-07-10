#pragma once

#include <cstdint>

#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseFileAccess.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

class HttpResponseBodyPlan final {
public:
    [[nodiscard]] const ResponseWritePolicy& policy() const noexcept {
        return policy_;
    }

    [[nodiscard]] bool statusAllowsBody() const noexcept {
        return policy_.bodyAllowed();
    }

    [[nodiscard]] bool bodySuppressed() const noexcept {
        return bodySuppressed_;
    }

private:
    friend HttpResponseBodyPlan httpResponseBodyPlan(HttpMethod, std::uint16_t) noexcept;
    friend class HttpBufferedResponseWritePlan;

    HttpResponseBodyPlan(ResponseWritePolicy policy, bool bodySuppressed) noexcept
        : policy_(policy), bodySuppressed_(bodySuppressed) {}

    ResponseWritePolicy policy_;
    bool bodySuppressed_{false};
};

[[nodiscard]] inline HttpResponseBodyPlan httpResponseBodyPlan(
    HttpMethod requestMethod,
    std::uint16_t statusCode) noexcept {
    const auto policy = responseWritePolicy(statusCode);
    return HttpResponseBodyPlan(
        policy,
        !policy.bodyAllowed() || requestMethod == HttpMethod::kHead);
}

class HttpBufferedResponseWritePlan final {
public:
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
        const HttpResponseBodyPlan&, const HttpResponse&) noexcept;

    HttpBufferedResponseWritePlan(
        HttpResponseBodyPlan bodyPlan,
        std::uint64_t contentLength) noexcept
        : bodyPlan_(bodyPlan), contentLength_(contentLength) {}

    HttpResponseBodyPlan bodyPlan_;
    std::uint64_t contentLength_{0};
};

[[nodiscard]] inline HttpBufferedResponseWritePlan httpBufferedResponseWritePlan(
    const HttpResponseBodyPlan& bodyPlan,
    const HttpResponse& response) noexcept {
    std::uint64_t contentLength = 0;
    if (bodyPlan.statusAllowsBody()) {
        contentLength = responseHasFileBody(response)
            ? responseFileBody(response).length
            : static_cast<std::uint64_t>(responseBodySize(response));
    }
    return HttpBufferedResponseWritePlan(bodyPlan, contentLength);
}

[[nodiscard]] inline HttpBufferedResponseWritePlan httpBufferedResponseWritePlan(
    HttpMethod requestMethod,
    const HttpResponse& response) noexcept {
    return httpBufferedResponseWritePlan(
        httpResponseBodyPlan(requestMethod, response.status()),
        response);
}

}  // namespace ruvia::detail
