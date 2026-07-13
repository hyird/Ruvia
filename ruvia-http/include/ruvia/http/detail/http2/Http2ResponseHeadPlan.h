#pragma once

#include <charconv>
#include <cstdint>
#include <system_error>
#include <variant>

#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

// HTTP/2 does not use Content-Length for frame delimiting, but RFC 9113
// section 8.1.1 still requires a declared value to agree with the DATA content
// unless message semantics forbid content. These alternatives make the owner of
// that field explicit before HPACK or stream state is mutated.
class Http2CanonicalResponseContentLength final {
public:
    explicit constexpr Http2CanonicalResponseContentLength(
        std::uint64_t value) noexcept
        : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

private:
    std::uint64_t value_{0};
};

class Http2ExplicitResponseContentLength final {
public:
    explicit constexpr Http2ExplicitResponseContentLength(
        std::uint64_t value) noexcept
        : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

private:
    std::uint64_t value_{0};
};

class Http2AbsentResponseContentLength final {};
class Http2ForbiddenResponseContentLength final {};

enum class Http2ResponseHeadPlanError : std::uint8_t {
    kInvalidContentLength,
    kConnectTunnelRequired,
    kResponseStatusMismatch,
    kResponseRepresentationMismatch
};

class Http2ResponseHeadPlanFailure final {
public:
    [[nodiscard]] constexpr Http2ResponseHeadPlanError error() const noexcept {
        return error_;
    }

private:
    friend class Http2ResponseHeadPlanResult;

    explicit constexpr Http2ResponseHeadPlanFailure(
        Http2ResponseHeadPlanError error) noexcept
        : error_(error) {}

    Http2ResponseHeadPlanError error_;
};

class Http2ResponseHeadPlanResult;

class Http2ResponseHeadPlan final {
public:
    [[nodiscard]] const HttpResponseBodyPlan& bodyPlan() const noexcept {
        return bodyPlan_;
    }

    [[nodiscard]] const Http2CanonicalResponseContentLength*
    canonicalContentLength() const noexcept {
        return std::get_if<Http2CanonicalResponseContentLength>(
            &contentLength_);
    }

    [[nodiscard]] const Http2ExplicitResponseContentLength*
    explicitContentLength() const noexcept {
        return std::get_if<Http2ExplicitResponseContentLength>(
            &contentLength_);
    }

    [[nodiscard]] const Http2AbsentResponseContentLength*
    absentContentLength() const noexcept {
        return std::get_if<Http2AbsentResponseContentLength>(
            &contentLength_);
    }

    [[nodiscard]] const Http2ForbiddenResponseContentLength*
    forbiddenContentLength() const noexcept {
        return std::get_if<Http2ForbiddenResponseContentLength>(
            &contentLength_);
    }

private:
    friend class Http2ResponseHeadPlanResult;

    using ContentLength = std::variant<
        Http2CanonicalResponseContentLength,
        Http2ExplicitResponseContentLength,
        Http2AbsentResponseContentLength,
        Http2ForbiddenResponseContentLength>;

    template <typename ContentLengthAlternative>
    Http2ResponseHeadPlan(
        HttpResponseBodyPlan bodyPlan,
        ContentLengthAlternative contentLength) noexcept
        : bodyPlan_(bodyPlan),
          contentLength_(contentLength) {}

    HttpResponseBodyPlan bodyPlan_;
    ContentLength contentLength_;
};

class Http2ResponseHeadPlanResult final {
public:
    [[nodiscard]] const Http2ResponseHeadPlan* plan() const noexcept {
        return std::get_if<Http2ResponseHeadPlan>(&value_);
    }

    [[nodiscard]] const Http2ResponseHeadPlanFailure* failure() const noexcept {
        return std::get_if<Http2ResponseHeadPlanFailure>(&value_);
    }

private:
    friend Http2ResponseHeadPlanResult http2BufferedResponseHeadPlan(
        const HttpBufferedResponseWritePlan&,
        const HttpResponse&) noexcept;
    friend Http2ResponseHeadPlanResult http2StreamingResponseHeadPlan(
        const HttpResponseBodyPlan&,
        const HttpResponse&) noexcept;
    friend Http2ResponseHeadPlanResult http2ConnectResponseHeadPlan(
        const HttpResponseBodyPlan&) noexcept;

    using Value = std::variant<
        Http2ResponseHeadPlan,
        Http2ResponseHeadPlanFailure>;

    template <typename Alternative>
    explicit Http2ResponseHeadPlanResult(Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static Http2ResponseHeadPlanResult canonical(
        HttpResponseBodyPlan bodyPlan,
        std::uint64_t value) noexcept {
        return Http2ResponseHeadPlanResult(
            Http2ResponseHeadPlan(
                bodyPlan,
                Http2CanonicalResponseContentLength(value)));
    }

    [[nodiscard]] static Http2ResponseHeadPlanResult preserveExplicit(
        HttpResponseBodyPlan bodyPlan,
        const HttpResponse& response) noexcept {
        if (!responseHasKnownHeader(
                response,
                kResponseHeaderContentLength)) {
            return Http2ResponseHeadPlanResult(
                Http2ResponseHeadPlan(
                    bodyPlan,
                    Http2AbsentResponseContentLength{}));
        }

        const auto value = responseKnownHeader(
            response,
            kResponseHeaderContentLength);
        std::uint64_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(
            value.data(), value.data() + value.size(), parsed);
        if (value.empty() || ec != std::errc{} ||
            ptr != value.data() + value.size()) {
            return Http2ResponseHeadPlanResult(
                Http2ResponseHeadPlanFailure(
                    Http2ResponseHeadPlanError::kInvalidContentLength));
        }
        return Http2ResponseHeadPlanResult(
            Http2ResponseHeadPlan(
                bodyPlan,
                Http2ExplicitResponseContentLength(parsed)));
    }

    [[nodiscard]] static Http2ResponseHeadPlanResult forbidden(
        HttpResponseBodyPlan bodyPlan) noexcept {
        return Http2ResponseHeadPlanResult(
            Http2ResponseHeadPlan(
                bodyPlan,
                Http2ForbiddenResponseContentLength{}));
    }

    [[nodiscard]] static Http2ResponseHeadPlanResult failure(
        Http2ResponseHeadPlanError error) noexcept {
        return Http2ResponseHeadPlanResult(
            Http2ResponseHeadPlanFailure(error));
    }

    Value value_;
};

[[nodiscard]] inline Http2ResponseHeadPlanResult
http2BufferedResponseHeadPlan(
    const HttpBufferedResponseWritePlan& writePlan,
    const HttpResponse& response) noexcept {
    const auto& bodyPlan = writePlan.bodyPlan();
    if (writePlan.responseStatus() != response.status()) {
        return Http2ResponseHeadPlanResult::failure(
            Http2ResponseHeadPlanError::kResponseStatusMismatch);
    }
    if (!writePlan.matchesResponse(response)) {
        return Http2ResponseHeadPlanResult::failure(
            Http2ResponseHeadPlanError::kResponseRepresentationMismatch);
    }
    const auto& policy = bodyPlan.policy();
    if (policy.autoContentLengthAllowed()) {
        return Http2ResponseHeadPlanResult::canonical(
            bodyPlan,
            policy.bodyAllowed() ? writePlan.contentLength() : 0);
    }
    return policy.explicitContentLengthAllowed()
        ? Http2ResponseHeadPlanResult::preserveExplicit(bodyPlan, response)
        : Http2ResponseHeadPlanResult::forbidden(bodyPlan);
}

[[nodiscard]] inline Http2ResponseHeadPlanResult
http2StreamingResponseHeadPlan(
    const HttpResponseBodyPlan& bodyPlan,
    const HttpResponse& response) noexcept {
    if (bodyPlan.responseStatus() != response.status()) {
        return Http2ResponseHeadPlanResult::failure(
            Http2ResponseHeadPlanError::kResponseStatusMismatch);
    }
    const auto& policy = bodyPlan.policy();
    if (policy.autoContentLengthAllowed() && !policy.bodyAllowed()) {
        return Http2ResponseHeadPlanResult::canonical(bodyPlan, 0);
    }
    return policy.explicitContentLengthAllowed()
        ? Http2ResponseHeadPlanResult::preserveExplicit(bodyPlan, response)
        : Http2ResponseHeadPlanResult::forbidden(bodyPlan);
}

[[nodiscard]] inline Http2ResponseHeadPlanResult
http2ConnectResponseHeadPlan(
    const HttpResponseBodyPlan& bodyPlan) noexcept {
    return bodyPlan.contentSemantics().connectTunnel() != nullptr
        ? Http2ResponseHeadPlanResult::forbidden(bodyPlan)
        : Http2ResponseHeadPlanResult::failure(
              Http2ResponseHeadPlanError::kConnectTunnelRequired);
}

}  // namespace ruvia::detail
