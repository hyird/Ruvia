#pragma once

#include <cstdint>
#include <type_traits>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseContentSemantics.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

class HttpResponseBodyPlan final {
public:
    [[nodiscard]] HttpKnownMethod requestMethod() const noexcept {
        return requestMethod_;
    }

    [[nodiscard]] HttpStatusCode responseStatus() const noexcept {
        return responseStatus_;
    }

    [[nodiscard]] ResponseWritePolicy policy() const noexcept {
        return policy_;
    }

    [[nodiscard]] bool statusAllowsBody() const noexcept {
        return policy_.bodyAllowed();
    }

    [[nodiscard]] bool bodySuppressed() const noexcept {
        return !policy_.bodyAllowed() ||
            semantics_ != HttpResponseContentSemantics::kWithContent;
    }

    [[nodiscard]] HttpResponseContentSemantics
    contentSemantics() const noexcept {
        return semantics_;
    }

    [[nodiscard]] std::uint64_t bufferedRepresentationLength(
        const HttpResponse& response) const noexcept {
        if (!statusAllowsBody() ||
            semantics_ == HttpResponseContentSemantics::kConnectTunnel) {
            return 0;
        }
        return static_cast<std::uint64_t>(responseBody(response).size());
    }

private:
    friend HttpResponseBodyPlan httpResponseBodyPlan(
        HttpKnownMethod, HttpStatusCode) noexcept;
    friend class HttpBufferedResponseWritePlan;

    constexpr HttpResponseBodyPlan(
        HttpKnownMethod requestMethod,
        HttpStatusCode responseStatus,
        ResponseWritePolicy policy,
        HttpResponseContentSemantics semantics) noexcept
        : requestMethod_(requestMethod),
          responseStatus_(responseStatus),
          policy_(policy),
          semantics_(semantics) {}

    HttpKnownMethod requestMethod_;
    HttpStatusCode responseStatus_;
    ResponseWritePolicy policy_;
    HttpResponseContentSemantics semantics_;
};

static_assert(std::is_trivially_copyable_v<HttpResponseBodyPlan>);
static_assert(sizeof(HttpResponseBodyPlan) <= 12);

[[nodiscard]] inline HttpResponseBodyPlan httpResponseBodyPlan(
    HttpKnownMethod requestMethod,
    HttpStatusCode statusCode) noexcept {
    const auto policy = responseWritePolicy(statusCode);
    return HttpResponseBodyPlan(
        requestMethod,
        statusCode,
        policy,
        httpResponseContentSemantics(requestMethod, statusCode));
}

class HttpBufferedResponseWritePlan final {
public:
    [[nodiscard]] HttpKnownMethod requestMethod() const noexcept {
        return bodyPlan_.requestMethod();
    }

    [[nodiscard]] HttpStatusCode responseStatus() const noexcept {
        return bodyPlan_.responseStatus();
    }

    [[nodiscard]] HttpResponseBodyPlan bodyPlan() const noexcept {
        return bodyPlan_;
    }

    [[nodiscard]] ResponseWritePolicy policy() const noexcept {
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

    // The response remains mutable after planning. Consumers validate this
    // snapshot before wire mutation so a changed status/body cannot silently
    // reuse stale representation metadata.
    [[nodiscard]] bool matchesResponse(
        const HttpResponse& response) const noexcept {
        return responseStatus() == response.status() &&
            contentLength_ ==
                bodyPlan_.bufferedRepresentationLength(response);
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
    return HttpBufferedResponseWritePlan(
        bodyPlan,
        bodyPlan.bufferedRepresentationLength(response));
}

}  // namespace ruvia::detail
