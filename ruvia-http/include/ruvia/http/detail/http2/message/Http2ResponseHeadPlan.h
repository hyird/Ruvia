#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <system_error>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

enum class Http2ResponseHeadPlanError : std::uint8_t { kInvalidContentLength, kConnectTunnelRequired, kResponseStatusMismatch, kResponseRepresentationMismatch };

class Http2ResponseHeadPlanFailure final {
public:
    [[nodiscard]] constexpr Http2ResponseHeadPlanError error() const noexcept {
        return error_;
    }

private:
    friend class Http2ResponseHeadPlanResult;

    explicit constexpr Http2ResponseHeadPlanFailure(Http2ResponseHeadPlanError error) noexcept
        : error_(error) {}

    Http2ResponseHeadPlanError error_;
};

class Http2ResponseHeadPlanResult;

class Http2ResponseHeadPlan final {
public:
    [[nodiscard]] HttpResponseBodyPlan bodyPlan() const noexcept {
        return bodyPlan_;
    }

    // The normalized Content-Length field to encode, if any. HTTP/2 framing
    // never depends on this value, but RFC 9113 requires any emitted value to
    // agree with the DATA content.
    [[nodiscard]] std::optional<std::uint64_t> contentLength() const noexcept {
        return contentLengthMode_ == ContentLengthMode::kOmit ? std::nullopt : std::optional<std::uint64_t>(contentLength_);
    }

    // Only an application-declared streaming length constrains subsequent DATA.
    // Framework-generated buffered lengths are already bound to their response
    // representation and therefore do not create a streaming accounting limit.
    [[nodiscard]] std::optional<std::uint64_t> streamingContentLength() const noexcept {
        return contentLengthMode_ == ContentLengthMode::kExplicit ? std::optional<std::uint64_t>(contentLength_) : std::nullopt;
    }

private:
    friend class Http2ResponseHeadPlanResult;

    enum class ContentLengthMode : std::uint8_t {
        kOmit,
        kCanonical,
        kExplicit,
    };

    Http2ResponseHeadPlan(HttpResponseBodyPlan bodyPlan, ContentLengthMode contentLengthMode, std::uint64_t contentLength = 0) noexcept
        : bodyPlan_(bodyPlan),
          contentLengthMode_(contentLengthMode),
          contentLength_(contentLength) {}

    HttpResponseBodyPlan bodyPlan_;
    ContentLengthMode contentLengthMode_;
    std::uint64_t contentLength_{0};
};

static_assert(std::is_trivially_copyable_v<Http2ResponseHeadPlan>);
static_assert(sizeof(Http2ResponseHeadPlan) <= 24);

class Http2ResponseHeadPlanResult final {
public:
    [[nodiscard]] const Http2ResponseHeadPlan* plan() const& noexcept {
        return std::get_if<Http2ResponseHeadPlan>(&value_);
    }
    [[nodiscard]] const Http2ResponseHeadPlan* plan() const&& = delete;

    [[nodiscard]] const Http2ResponseHeadPlanFailure* failure() const& noexcept {
        return std::get_if<Http2ResponseHeadPlanFailure>(&value_);
    }
    [[nodiscard]] const Http2ResponseHeadPlanFailure* failure() const&& = delete;

private:
    friend Http2ResponseHeadPlanResult http2BufferedResponseHeadPlan(const HttpBufferedResponseWritePlan&, const HttpResponse&) noexcept;
    friend Http2ResponseHeadPlanResult http2StreamingResponseHeadPlan(const HttpResponseBodyPlan&, const HttpResponse&) noexcept;
    friend Http2ResponseHeadPlanResult http2ConnectResponseHeadPlan(const HttpResponseBodyPlan&) noexcept;

    using Value = std::variant<Http2ResponseHeadPlan, Http2ResponseHeadPlanFailure>;

    template <typename Alternative>
    explicit Http2ResponseHeadPlanResult(Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static Http2ResponseHeadPlanResult canonical(HttpResponseBodyPlan bodyPlan, std::uint64_t value) noexcept {
        return Http2ResponseHeadPlanResult(Http2ResponseHeadPlan(bodyPlan, Http2ResponseHeadPlan::ContentLengthMode::kCanonical, value));
    }

    [[nodiscard]] static Http2ResponseHeadPlanResult preserveExplicit(HttpResponseBodyPlan bodyPlan, const HttpResponse& response) noexcept {
        if (!responseHasKnownHeader(response, kResponseHeaderContentLength)) {
            return omit(bodyPlan);
        }

        const auto value = responseKnownHeader(response, kResponseHeaderContentLength);
        std::uint64_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size()) {
            return Http2ResponseHeadPlanResult(Http2ResponseHeadPlanFailure(Http2ResponseHeadPlanError::kInvalidContentLength));
        }
        return Http2ResponseHeadPlanResult(Http2ResponseHeadPlan(bodyPlan, Http2ResponseHeadPlan::ContentLengthMode::kExplicit, parsed));
    }

    [[nodiscard]] static Http2ResponseHeadPlanResult omit(HttpResponseBodyPlan bodyPlan) noexcept {
        return Http2ResponseHeadPlanResult(Http2ResponseHeadPlan(bodyPlan, Http2ResponseHeadPlan::ContentLengthMode::kOmit));
    }

    [[nodiscard]] static Http2ResponseHeadPlanResult failure(Http2ResponseHeadPlanError error) noexcept {
        return Http2ResponseHeadPlanResult(Http2ResponseHeadPlanFailure(error));
    }

    Value value_;
};

[[nodiscard]] inline Http2ResponseHeadPlanResult http2BufferedResponseHeadPlan(const HttpBufferedResponseWritePlan& writePlan, const HttpResponse& response) noexcept {
    const auto bodyPlan = writePlan.bodyPlan();
    if (writePlan.responseStatus() != response.status()) {
        return Http2ResponseHeadPlanResult::failure(Http2ResponseHeadPlanError::kResponseStatusMismatch);
    }
    if (!writePlan.matchesResponse(response)) {
        return Http2ResponseHeadPlanResult::failure(Http2ResponseHeadPlanError::kResponseRepresentationMismatch);
    }
    const auto policy = bodyPlan.policy();
    if (policy.autoContentLengthAllowed()) {
        return Http2ResponseHeadPlanResult::canonical(bodyPlan, policy.bodyAllowed() ? writePlan.contentLength() : 0);
    }
    return policy.explicitContentLengthAllowed() ? Http2ResponseHeadPlanResult::preserveExplicit(bodyPlan, response) : Http2ResponseHeadPlanResult::omit(bodyPlan);
}

[[nodiscard]] inline Http2ResponseHeadPlanResult http2StreamingResponseHeadPlan(const HttpResponseBodyPlan& bodyPlan, const HttpResponse& response) noexcept {
    if (bodyPlan.responseStatus() != response.status()) {
        return Http2ResponseHeadPlanResult::failure(Http2ResponseHeadPlanError::kResponseStatusMismatch);
    }
    const auto policy = bodyPlan.policy();
    if (policy.autoContentLengthAllowed() && !policy.bodyAllowed()) {
        return Http2ResponseHeadPlanResult::canonical(bodyPlan, 0);
    }
    return policy.explicitContentLengthAllowed() ? Http2ResponseHeadPlanResult::preserveExplicit(bodyPlan, response) : Http2ResponseHeadPlanResult::omit(bodyPlan);
}

[[nodiscard]] inline Http2ResponseHeadPlanResult http2ConnectResponseHeadPlan(const HttpResponseBodyPlan& bodyPlan) noexcept {
    return bodyPlan.contentSemantics() == HttpResponseContentSemantics::kConnectTunnel ? Http2ResponseHeadPlanResult::omit(bodyPlan) : Http2ResponseHeadPlanResult::failure(Http2ResponseHeadPlanError::kConnectTunnelRequired);
}

}  // namespace ruvia::detail
