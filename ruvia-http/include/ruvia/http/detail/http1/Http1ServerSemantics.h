#pragma once

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/detail/HttpConnectionFields.h"
#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
#include "ruvia/http/detail/server/HttpFinalResponseControlPlan.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/HttpResponse.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace ruvia::detail {

// Runtime-owned policy that is independent of HTTP message semantics (for
// example, a per-connection request limit). A named policy keeps the Web driver
// from passing an unlabelled bool into the protocol planner.
enum class Http1ServerClosePolicy : std::uint8_t {
    kAllowReuse,
    kCloseAfterResponse
};

[[nodiscard]] inline constexpr Http1ServerConnectionPlan
http1ApplyRequestBodyConsumption(
    Http1ServerConnectionPlan plan,
    Http1RequestBodyConsumption consumption) noexcept {
    return consumption == Http1RequestBodyConsumption::kComplete
        ? plan
        : plan.requireClose();
}

class Http1ResponseStreamPlan final {
public:
    [[nodiscard]] ResponseStreamFraming framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] Http1ServerConnectionPlan requestConnectionPlan() const noexcept {
        return requestConnectionPlan_;
    }

    [[nodiscard]] Http1ServerClosePolicy closePolicy() const noexcept {
        return closePolicy_;
    }

    [[nodiscard]] HttpKnownMethod requestMethod() const noexcept {
        return requestMethod_;
    }

private:
    friend Http1ResponseStreamPlan http1PlanResponseStream(
        const Http1ServerRequestParseState&, Http1ServerClosePolicy) noexcept;

    Http1ResponseStreamPlan(
        ResponseStreamFraming framing,
        Http1ServerConnectionPlan requestConnectionPlan,
        Http1ServerClosePolicy closePolicy,
        HttpKnownMethod requestMethod) noexcept
        : framing_(framing),
          requestConnectionPlan_(requestConnectionPlan),
          closePolicy_(closePolicy),
          requestMethod_(requestMethod) {}

    ResponseStreamFraming framing_;
    Http1ServerConnectionPlan requestConnectionPlan_;
    Http1ServerClosePolicy closePolicy_{Http1ServerClosePolicy::kCloseAfterResponse};
    HttpKnownMethod requestMethod_{HttpKnownMethod::kUnknown};
};

// Pure HTTP/1 response-stream planning. The runtime contributes only its typed
// product policy (for example, a per-connection request limit); HTTP retains the
// request disposition and candidate framing until the response status is known.
// Commit can therefore distinguish a body-allowed HTTP/1.0 stream, which requires
// close delimiting, from a body-suppressed response that is already self-delimited.
[[nodiscard]] inline Http1ResponseStreamPlan http1PlanResponseStream(
    const Http1ServerRequestParseState& parsed,
    Http1ServerClosePolicy closePolicy) noexcept {
    const auto requestConnectionPlan = http1ApplyRequestBodyConsumption(
        parsed.connectionPlan,
        parsed.bodyPlan.requiresConsumption()
            ? Http1RequestBodyConsumption::kIncomplete
            : Http1RequestBodyConsumption::kComplete);
    const auto framing =
        parsed.request.protocolVersion() == HttpProtocolVersion::kHttp11
        ? ResponseStreamFraming::kHttp1Chunked
        : ResponseStreamFraming::kHttp1CloseDelimited;
    return Http1ResponseStreamPlan(
        framing,
        requestConnectionPlan,
        closePolicy,
        parsed.request.knownMethod());
}

enum class Http1ConnectionCloseFieldPolicy : std::uint8_t {
    kCloseOnly,
    kPreserveUpgrade
};

inline void http1MarkConnectionClose(
    HttpResponse& response,
    Http1ConnectionCloseFieldPolicy fieldPolicy =
        Http1ConnectionCloseFieldPolicy::kCloseOnly) {
    // A runtime close verdict dominates keep-alive. Collapse repeated fields
    // after the socket lifecycle is decided, while preserving the Upgrade
    // option required by any retained Upgrade field.
    response.header("Connection", std::nullopt);
    setResponseHeaderStableView(
        response,
        "Connection",
        fieldPolicy == Http1ConnectionCloseFieldPolicy::kPreserveUpgrade
            ? "close, Upgrade"
            : "close");
}

class Http1FinalResponseCommitResult;

class Http1FinalResponseCommit final {
public:
    [[nodiscard]] Http1ServerConnectionPlan connectionPlan() const noexcept {
        return connectionPlan_;
    }

private:
    friend class Http1FinalResponseCommitResult;

    explicit Http1FinalResponseCommit(
        Http1ServerConnectionPlan connectionPlan) noexcept
        : connectionPlan_(connectionPlan) {}

    Http1ServerConnectionPlan connectionPlan_;
};

class Http1FinalResponseCommitFailure final {
public:
    [[nodiscard]] HttpFinalResponseControlPlanError error() const noexcept {
        return error_;
    }

private:
    friend class Http1FinalResponseCommitResult;

    explicit Http1FinalResponseCommitFailure(
        HttpFinalResponseControlPlanError error) noexcept
        : error_(error) {}

    HttpFinalResponseControlPlanError error_;
};

// A final response commit either owns the authoritative connection contract or
// one typed message failure. Validation completes before Connection is mutated,
// so callers cannot observe a half-committed response after a protocol failure.
class Http1FinalResponseCommitResult final {
public:
    [[nodiscard]] const Http1FinalResponseCommit* committed() const & noexcept {
        return std::get_if<Http1FinalResponseCommit>(&value_);
    }
    [[nodiscard]] const Http1FinalResponseCommit* committed() const && = delete;

    [[nodiscard]] const Http1FinalResponseCommitFailure*
    failure() const & noexcept {
        return std::get_if<Http1FinalResponseCommitFailure>(&value_);
    }
    [[nodiscard]] const Http1FinalResponseCommitFailure*
    failure() const && = delete;

private:
    friend Http1FinalResponseCommitResult http1CommitFinalResponse(
        HttpResponse&, Http1ServerConnectionPlan);

    using Value = std::variant<
        Http1FinalResponseCommit,
        Http1FinalResponseCommitFailure>;

    template <typename Alternative>
    explicit Http1FinalResponseCommitResult(Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static Http1FinalResponseCommitResult committed(
        Http1ServerConnectionPlan connectionPlan) noexcept {
        return Http1FinalResponseCommitResult(
            Http1FinalResponseCommit(connectionPlan));
    }

    [[nodiscard]] static Http1FinalResponseCommitResult failure(
        HttpFinalResponseControlPlanError error) noexcept {
        return Http1FinalResponseCommitResult(
            Http1FinalResponseCommitFailure(error));
    }

    Value value_;
};

// Commit response-side HTTP/1 persistence after the runtime has folded in
// request-body completion and server policy. This is the sole protocol mutation:
// it honors an application-provided Connection: close and emits the version-
// appropriate Connection field. Success retains the exact request version for
// head serialization; wire-message failures remain typed.
[[nodiscard]] inline Http1FinalResponseCommitResult http1CommitFinalResponse(
    HttpResponse& response,
    Http1ServerConnectionPlan plan) {
    const auto controlResult = httpFinalResponseControlPlan(
        response,
        plan.protocolVersion());
    if (const auto* failure = controlResult.failure()) {
        return Http1FinalResponseCommitResult::failure(failure->error());
    }
    const auto* controlPlan = controlResult.plan();
    // Http1ServerConnectionPlan can only retain HTTP/1.0 or HTTP/1.1, so the
    // shared control planner must produce its HTTP/1 alternative here.
    const auto& http1Control = *controlPlan->http1();
    const auto& responseOptions = http1Control.connectionOptions();
    const auto& upgradeProtocols = http1Control.upgradeProtocols();
    const bool preserveUpgrade = upgradeProtocols.hasField();
    const bool generateUpgradeOption =
        preserveUpgrade && !responseOptions.upgrade();
    if (generateUpgradeOption) {
        if (responseOptions.hasField()) {
            response.header(
                "Connection",
                "Upgrade",
                HttpResponse::HeaderOptions{.append = true});
        } else {
            setResponseHeaderStableView(response, "Connection", "Upgrade");
        }
    }
    if (responseOptions.close()) {
        plan = plan.requireClose();
    }
    if (plan.disposition() == Http1ConnectionDisposition::kClose) {
        http1MarkConnectionClose(
            response,
            preserveUpgrade
                ? Http1ConnectionCloseFieldPolicy::kPreserveUpgrade
                : Http1ConnectionCloseFieldPolicy::kCloseOnly);
    } else if (plan.protocolVersion() == HttpProtocolVersion::kHttp10 &&
               !responseOptions.keepAlive()) {
        if (responseOptions.hasField() || generateUpgradeOption) {
            response.header(
                "Connection",
                "keep-alive",
                HttpResponse::HeaderOptions{.append = true});
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
    [[nodiscard]] HttpResponse& response() noexcept {
        return head_.response();
    }

    [[nodiscard]] const HttpResponse& response() const noexcept {
        return head_.response();
    }

    [[nodiscard]] const Http1ResponseHeadPlan& responseHeadPlan() const noexcept {
        return responseHeadPlan_;
    }

    [[nodiscard]] const ResponseStreamCommitPlan& commitPlan() const noexcept {
        return head_.commitPlan();
    }

    [[nodiscard]] Http1ServerConnectionPlan connectionPlan() const noexcept {
        return connectionPlan_;
    }

private:
    friend class PreparedHttp1ResponseStreamResult;
    friend PreparedHttp1ResponseStreamResult prepareHttp1ResponseStreamHead(
        HttpResponse,
        ResponseStreamKind,
        const Http1ResponseStreamPlan&,
        ResponseTrailerIntent);

    PreparedHttp1ResponseStream(
        ResponseStreamHead head,
        Http1ResponseHeadPlan responseHeadPlan,
        Http1ServerConnectionPlan connectionPlan) noexcept
        : head_(std::move(head)),
          responseHeadPlan_(responseHeadPlan),
          connectionPlan_(connectionPlan) {}

    ResponseStreamHead head_;
    Http1ResponseHeadPlan responseHeadPlan_;
    Http1ServerConnectionPlan connectionPlan_;
};

class PreparedHttp1ResponseStreamResult final {
public:
    [[nodiscard]] const PreparedHttp1ResponseStream*
    prepared() const & noexcept {
        return std::get_if<PreparedHttp1ResponseStream>(&value_);
    }
    [[nodiscard]] const PreparedHttp1ResponseStream*
    prepared() const && = delete;

    [[nodiscard]] PreparedHttp1ResponseStream* prepared() & noexcept {
        return std::get_if<PreparedHttp1ResponseStream>(&value_);
    }
    [[nodiscard]] PreparedHttp1ResponseStream* prepared() && = delete;

    [[nodiscard]] const Http1FinalResponseCommitFailure*
    failure() const & noexcept {
        return std::get_if<Http1FinalResponseCommitFailure>(&value_);
    }
    [[nodiscard]] const Http1FinalResponseCommitFailure*
    failure() const && = delete;

    [[nodiscard]] PreparedHttp1ResponseStream takePrepared() && {
        return std::move(std::get<PreparedHttp1ResponseStream>(value_));
    }

private:
    friend PreparedHttp1ResponseStreamResult prepareHttp1ResponseStreamHead(
        HttpResponse,
        ResponseStreamKind,
        const Http1ResponseStreamPlan&,
        ResponseTrailerIntent);

    using Value = std::variant<
        PreparedHttp1ResponseStream,
        Http1FinalResponseCommitFailure>;

    template <typename Alternative>
    explicit PreparedHttp1ResponseStreamResult(Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

[[nodiscard]] inline PreparedHttp1ResponseStreamResult
prepareHttp1ResponseStreamHead(
    HttpResponse response,
    ResponseStreamKind kind,
    const Http1ResponseStreamPlan& plan,
    ResponseTrailerIntent trailerIntent) {
    auto commitPlan = httpResponseStreamCommitPlan(
        plan.framing(),
        plan.requestMethod(),
        response.status(),
        trailerIntent);
    const auto& bodyPlan = commitPlan.bodyPlan();
    // HTTP/1.0 cannot delimit an open-ended response stream without closing the
    // connection, but a response whose method/status forbids payload is already
    // self-delimited. Make this decision at head commit, when the response status is
    // finally known, instead of pessimistically baking close into the pre-commit plan.
    const auto plannedConnection =
        plan.requestConnectionPlan().disposition() == Http1ConnectionDisposition::kReuse &&
            plan.closePolicy() == Http1ServerClosePolicy::kAllowReuse &&
            (plan.framing() != ResponseStreamFraming::kHttp1CloseDelimited ||
             bodyPlan.bodySuppressed())
        ? plan.requestConnectionPlan()
        : plan.requestConnectionPlan().requireClose();
    const auto commitResult = http1CommitFinalResponse(
        response,
        plannedConnection);
    if (const auto* failure = commitResult.failure()) {
        return PreparedHttp1ResponseStreamResult(*failure);
    }
    const auto connectionPlan =
        commitResult.committed()->connectionPlan();
    auto head = prepareResponseStreamHead(
        std::move(response), kind, std::move(commitPlan));
    const auto responseHeadPlan =
        plan.framing() == ResponseStreamFraming::kHttp1Chunked
        ? http1ChunkedResponseStreamHeadPlan(
              head.commitPlan().bodyPlan(),
              connectionPlan)
        : http1CloseDelimitedResponseStreamHeadPlan(
              head.commitPlan().bodyPlan(),
              connectionPlan);
    return PreparedHttp1ResponseStreamResult(
        PreparedHttp1ResponseStream(
            std::move(head),
            responseHeadPlan,
            connectionPlan));
}

}  // namespace ruvia::detail
