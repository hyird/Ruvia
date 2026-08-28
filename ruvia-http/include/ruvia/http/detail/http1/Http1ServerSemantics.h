#pragma once

#include "ruvia/http/Http1ClosePolicy.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
#include "ruvia/http/detail/server/HttpFinalResponseControlPlan.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/HttpResponse.h"

#include <cstdint>
#include <exception>
#include <optional>
#include <utility>
#include <variant>

namespace ruvia::detail {

[[nodiscard]] inline constexpr Http1ServerConnectionPlan http1ApplyRequestBodyConsumption(Http1ServerConnectionPlan plan, Http1RequestBodyConsumption consumption) noexcept {
    return consumption == Http1RequestBodyConsumption::kComplete ? plan : plan.requireClose();
}

class Http1ResponseStreamPlan final {
public:
    [[nodiscard]] ResponseStreamFraming framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] Http1ServerConnectionPlan requestConnectionPlan() const noexcept {
        return requestConnectionPlan_;
    }

    [[nodiscard]] Http1ClosePolicy closePolicy() const noexcept {
        return closePolicy_;
    }

    [[nodiscard]] HttpKnownMethod requestMethod() const noexcept {
        return requestMethod_;
    }

private:
    friend Http1ResponseStreamPlan http1PlanResponseStream(const Http1ServerRequestParseState&, Http1ClosePolicy) noexcept;
    friend Http1ResponseStreamPlan http1PlanConsumedResponseStream(const Http1ServerRequestParseState&, Http1ClosePolicy) noexcept;

    Http1ResponseStreamPlan(ResponseStreamFraming framing, Http1ServerConnectionPlan requestConnectionPlan, Http1ClosePolicy closePolicy, HttpKnownMethod requestMethod) noexcept
        : framing_(framing),
          requestConnectionPlan_(requestConnectionPlan),
          closePolicy_(closePolicy),
          requestMethod_(requestMethod) {}

    ResponseStreamFraming framing_;
    Http1ServerConnectionPlan requestConnectionPlan_;
    Http1ClosePolicy closePolicy_{Http1ClosePolicy::kCloseAfterResponse};
    HttpKnownMethod requestMethod_{HttpKnownMethod::kUnknown};
};

// Pure HTTP/1 response-stream planning. The runtime contributes only its typed
// product policy (for example, a per-connection request limit); HTTP retains the
// request disposition and candidate framing until the response status is known.
// Commit can therefore distinguish a body-allowed HTTP/1.0 stream, which requires
// close delimiting, from a body-suppressed response that is already self-delimited.
[[nodiscard]] inline Http1ResponseStreamPlan http1PlanResponseStream(const Http1ServerRequestParseState& parsed, Http1ClosePolicy closePolicy) noexcept {
    const auto requestConnectionPlan = http1ApplyRequestBodyConsumption(parsed.connectionPlan, parsed.bodyPlan.requiresConsumption() ? Http1RequestBodyConsumption::kIncomplete : Http1RequestBodyConsumption::kComplete);
    const auto framing = parsed.request.protocolVersion() == HttpProtocolVersion::kHttp11 ? ResponseStreamFraming::kHttp1Chunked : ResponseStreamFraming::kHttp1CloseDelimited;
    return Http1ResponseStreamPlan(framing, requestConnectionPlan, closePolicy, parsed.request.knownMethod());
}

// Runtime variant for a route that buffered and consumed the complete request
// body before deciding to stream its response. The parser's body plan still
// describes the original request framing, so this named entry point records the
// completed consumption instead of pessimistically forcing connection close.
[[nodiscard]] inline Http1ResponseStreamPlan http1PlanConsumedResponseStream(const Http1ServerRequestParseState& parsed, Http1ClosePolicy closePolicy) noexcept {
    const auto framing = parsed.request.protocolVersion() == HttpProtocolVersion::kHttp11 ? ResponseStreamFraming::kHttp1Chunked : ResponseStreamFraming::kHttp1CloseDelimited;
    return Http1ResponseStreamPlan(framing, parsed.connectionPlan, closePolicy, parsed.request.knownMethod());
}

enum class Http1ConnectionCloseFieldPolicy : std::uint8_t { kCloseOnly, kPreserveUpgrade };

inline void http1MarkConnectionClose(HttpResponse& response, Http1ConnectionCloseFieldPolicy fieldPolicy = Http1ConnectionCloseFieldPolicy::kCloseOnly) {
    // A runtime close verdict dominates keep-alive. Collapse repeated fields
    // after the socket lifecycle is decided, while preserving the Upgrade
    // option required by any retained Upgrade field.
    response.removeHeader("Connection");
    setResponseHeaderStableView(response, "Connection", fieldPolicy == Http1ConnectionCloseFieldPolicy::kPreserveUpgrade ? "close, Upgrade" : "close");
}

class Http1FinalResponseCommitResult;
class Http1FinalResponseCommitFailure;

class Http1FinalResponseCommitError final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        switch (error_) {
            case Http1FinalResponseControlPlanError::kInvalidStatus:
                return "invalid final HTTP response status";
            case Http1FinalResponseControlPlanError::kInvalidConnectionField:
                return "invalid HTTP Connection header";
            case Http1FinalResponseControlPlanError::kInvalidUpgradeField:
                return "invalid HTTP Upgrade header";
            case Http1FinalResponseControlPlanError::kUpgradeRequired:
                return "Upgrade Required response requires an Upgrade protocol";
            case Http1FinalResponseControlPlanError::kTeFieldForbidden:
                return "TE is not a response field";
        }
        return "unknown HTTP final response commit failure";
    }

private:
    friend class Http1FinalResponseCommitFailure;

    explicit Http1FinalResponseCommitError(Http1FinalResponseControlPlanError error) noexcept
        : error_(error) {}

    Http1FinalResponseControlPlanError error_;
};

class Http1FinalResponseCommitFailure final {
public:
    [[nodiscard]] Http1FinalResponseCommitError exception() const noexcept {
        return Http1FinalResponseCommitError(error_);
    }

private:
    friend class Http1FinalResponseCommitResult;

    explicit Http1FinalResponseCommitFailure(const Http1FinalResponseControlPlanFailure& failure) noexcept
        : error_(failure.error_) {}

    Http1FinalResponseControlPlanError error_;
};

// A final response commit directly owns the authoritative connection contract
// or one typed message failure. Validation completes before Connection is
// mutated, so callers cannot observe a half-committed response after a protocol
// failure or unwrap a second success container.
class Http1FinalResponseCommitResult final {
public:
    [[nodiscard]] const Http1ServerConnectionPlan* committed() const& noexcept {
        return std::get_if<Http1ServerConnectionPlan>(&value_);
    }
    [[nodiscard]] const Http1ServerConnectionPlan* committed() const&& = delete;

    [[nodiscard]] const Http1FinalResponseCommitFailure* failure() const& noexcept {
        return std::get_if<Http1FinalResponseCommitFailure>(&value_);
    }
    [[nodiscard]] const Http1FinalResponseCommitFailure* failure() const&& = delete;

private:
    friend Http1FinalResponseCommitResult http1CommitFinalResponse(HttpResponse&, Http1ServerConnectionPlan);

    using Value = std::variant<Http1ServerConnectionPlan, Http1FinalResponseCommitFailure>;

    template <typename Alternative>
    explicit Http1FinalResponseCommitResult(Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static Http1FinalResponseCommitResult committed(Http1ServerConnectionPlan connectionPlan) noexcept {
        return Http1FinalResponseCommitResult(connectionPlan);
    }

    [[nodiscard]] static Http1FinalResponseCommitResult failure(const Http1FinalResponseControlPlanFailure& failure) noexcept {
        return Http1FinalResponseCommitResult(Http1FinalResponseCommitFailure(failure));
    }

    Value value_;
};

// Commit response-side HTTP/1 persistence after the runtime has folded in
// request-body completion and server policy. This is the sole protocol mutation:
// it honors an application-provided Connection: close and emits the version-
// appropriate Connection field. Success retains the exact request version for
// head serialization; wire-message failures remain typed.
[[nodiscard]] inline Http1FinalResponseCommitResult http1CommitFinalResponse(HttpResponse& response, Http1ServerConnectionPlan plan) {
    const auto controlResult = http1FinalResponseControlPlan(response);
    if (const auto* failure = controlResult.failure()) {
        return Http1FinalResponseCommitResult::failure(*failure);
    }
    const auto& http1Control = *controlResult.control();
    const auto responseOptions = http1Control.connectionOptions();
    const auto upgradeProtocols = http1Control.upgradeProtocols();
    const bool preserveUpgrade = upgradeProtocols.hasField();
    const bool generateUpgradeOption = preserveUpgrade && !responseOptions.upgrade();
    if (generateUpgradeOption) {
        if (responseOptions.hasField()) {
            response.header("Connection", "Upgrade", HttpResponse::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend});
        } else {
            setResponseHeaderStableView(response, "Connection", "Upgrade");
        }
    }
    if (responseOptions.close()) {
        plan = plan.requireClose();
    }
    if (plan.disposition() == Http1ClosePolicy::kCloseAfterResponse) {
        http1MarkConnectionClose(response, preserveUpgrade ? Http1ConnectionCloseFieldPolicy::kPreserveUpgrade : Http1ConnectionCloseFieldPolicy::kCloseOnly);
    } else if (plan.protocolVersion() == HttpProtocolVersion::kHttp10 && !responseOptions.keepAlive()) {
        if (responseOptions.hasField() || generateUpgradeOption) {
            response.header("Connection", "keep-alive", HttpResponse::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend});
        } else {
            setResponseHeaderStableView(response, "Connection", "keep-alive");
        }
    }
    return Http1FinalResponseCommitResult::committed(plan);
}

class PreparedHttp1ResponseStreamResult;

// Commit-time result. This object combines request/version/runtime constraints with
// the response method/status and Connection options, then binds the exact header
// bytes to the authoritative connection disposition.
class PreparedHttp1ResponseStream final {
public:
    [[nodiscard]] HttpResponse& response() & noexcept {
        return head_.response();
    }
    [[nodiscard]] HttpResponse& response() && = delete;

    [[nodiscard]] const HttpResponse& response() const& noexcept {
        return head_.response();
    }
    [[nodiscard]] const HttpResponse& response() const&& = delete;

    [[nodiscard]] const Http1ResponseHeadPlan& responseHeadPlan() const& noexcept {
        return responseHeadPlan_;
    }
    [[nodiscard]] const Http1ResponseHeadPlan& responseHeadPlan() const&& = delete;

    [[nodiscard]] const ResponseStreamCommitPlan& commitPlan() const& noexcept {
        return head_.commitPlan();
    }
    [[nodiscard]] const ResponseStreamCommitPlan& commitPlan() const&& = delete;

    [[nodiscard]] Http1ServerConnectionPlan connectionPlan() const noexcept {
        return connectionPlan_;
    }

private:
    friend class PreparedHttp1ResponseStreamResult;
    friend PreparedHttp1ResponseStreamResult prepareHttp1ResponseStreamHead(HttpResponse, ResponseStreamKind, const Http1ResponseStreamPlan&, ResponseTrailerIntent);
    friend PreparedHttp1ResponseStreamResult prepareHttp1KnownLengthResponseStreamHead(HttpResponse, std::uint64_t, ResponseStreamKind, const Http1ResponseStreamPlan&);

    PreparedHttp1ResponseStream(ResponseStreamHead head, Http1ResponseHeadPlan responseHeadPlan, Http1ServerConnectionPlan connectionPlan) noexcept
        : head_(std::move(head)),
          responseHeadPlan_(responseHeadPlan),
          connectionPlan_(connectionPlan) {}

    ResponseStreamHead head_;
    Http1ResponseHeadPlan responseHeadPlan_;
    Http1ServerConnectionPlan connectionPlan_;
};

class PreparedHttp1ResponseStreamResult final {
public:
    [[nodiscard]] const PreparedHttp1ResponseStream* prepared() const& noexcept {
        return std::get_if<PreparedHttp1ResponseStream>(&value_);
    }
    [[nodiscard]] const PreparedHttp1ResponseStream* prepared() const&& = delete;

    [[nodiscard]] PreparedHttp1ResponseStream* prepared() & noexcept {
        return std::get_if<PreparedHttp1ResponseStream>(&value_);
    }
    [[nodiscard]] PreparedHttp1ResponseStream* prepared() && = delete;

    [[nodiscard]] const Http1FinalResponseCommitFailure* failure() const& noexcept {
        return std::get_if<Http1FinalResponseCommitFailure>(&value_);
    }
    [[nodiscard]] const Http1FinalResponseCommitFailure* failure() const&& = delete;

private:
    friend PreparedHttp1ResponseStreamResult prepareHttp1ResponseStreamHead(HttpResponse, ResponseStreamKind, const Http1ResponseStreamPlan&, ResponseTrailerIntent);
    friend PreparedHttp1ResponseStreamResult prepareHttp1KnownLengthResponseStreamHead(HttpResponse, std::uint64_t, ResponseStreamKind, const Http1ResponseStreamPlan&);

    using Value = std::variant<PreparedHttp1ResponseStream, Http1FinalResponseCommitFailure>;

    template <typename Alternative>
    explicit PreparedHttp1ResponseStreamResult(Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

[[nodiscard]] inline PreparedHttp1ResponseStreamResult prepareHttp1ResponseStreamHead(HttpResponse response, ResponseStreamKind kind, const Http1ResponseStreamPlan& plan, ResponseTrailerIntent trailerIntent) {
    auto commitPlan = httpResponseStreamCommitPlan(plan.framing(), plan.requestMethod(), response.status(), trailerIntent);
    const auto bodyPlan = commitPlan.bodyPlan();
    // HTTP/1.0 cannot delimit an open-ended response stream without closing the
    // connection, but a response whose method/status forbids payload is already
    // self-delimited. Make this decision at head commit, when the response status is
    // finally known, instead of pessimistically baking close into the pre-commit plan.
    const auto plannedConnection = plan.requestConnectionPlan().disposition() == Http1ClosePolicy::kAllowReuse && plan.closePolicy() == Http1ClosePolicy::kAllowReuse && (plan.framing() != ResponseStreamFraming::kHttp1CloseDelimited || bodyPlan.bodySuppressed()) ? plan.requestConnectionPlan() : plan.requestConnectionPlan().requireClose();
    const auto commitResult = http1CommitFinalResponse(response, plannedConnection);
    if (const auto* failure = commitResult.failure()) {
        return PreparedHttp1ResponseStreamResult(*failure);
    }
    const auto connectionPlan = *commitResult.committed();
    auto head = prepareResponseStreamHead(std::move(response), kind, std::move(commitPlan));
    const auto responseHeadPlan = plan.framing() == ResponseStreamFraming::kHttp1Chunked ? http1ChunkedResponseStreamHeadPlan(head.commitPlan().bodyPlan(), connectionPlan) : http1CloseDelimitedResponseStreamHeadPlan(head.commitPlan().bodyPlan(), connectionPlan);
    return PreparedHttp1ResponseStreamResult(PreparedHttp1ResponseStream(std::move(head), responseHeadPlan, connectionPlan));
}

// Commit an incrementally written response whose decoded representation length
// is already known from the upstream protocol. The shared body policy still
// suppresses payload for HEAD/204/304, while the HTTP/1 head owns a canonical
// length and the runtime may keep the connection reusable on either version.
[[nodiscard]] inline PreparedHttp1ResponseStreamResult prepareHttp1KnownLengthResponseStreamHead(HttpResponse response, std::uint64_t contentLength, ResponseStreamKind kind, const Http1ResponseStreamPlan& plan) {
    auto commitPlan = httpResponseStreamCommitPlan(ResponseStreamFraming::kHttp1KnownLength, plan.requestMethod(), response.status(), ResponseTrailerIntent::kNone);
    const auto plannedConnection = plan.requestConnectionPlan().disposition() == Http1ClosePolicy::kAllowReuse && plan.closePolicy() == Http1ClosePolicy::kAllowReuse ? plan.requestConnectionPlan() : plan.requestConnectionPlan().requireClose();
    const auto commitResult = http1CommitFinalResponse(response, plannedConnection);
    if (const auto* failure = commitResult.failure()) {
        return PreparedHttp1ResponseStreamResult(*failure);
    }
    const auto connectionPlan = *commitResult.committed();
    auto head = prepareResponseStreamHead(std::move(response), kind, std::move(commitPlan));
    const auto responseHeadPlan = http1KnownLengthResponseStreamHeadPlan(head.commitPlan().bodyPlan(), connectionPlan, contentLength);
    return PreparedHttp1ResponseStreamResult(PreparedHttp1ResponseStream(std::move(head), responseHeadPlan, connectionPlan));
}

}  // namespace ruvia::detail
