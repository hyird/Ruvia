#pragma once

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
#include <stdexcept>
#include <utility>

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

// Connection is a list field and a response can contain repeated field lines.
// Inspect every line: the response's indexed known-header fast path intentionally
// points at only one occurrence and cannot decide transport lifecycle by itself.
[[nodiscard]] inline HttpConnectionOptions http1ResponseConnectionOptions(
    const HttpResponse& response) {
    HttpConnectionOptions options;
    for (const auto& header : response.headers()) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "Connection")) {
            continue;
        }
        if (options.parseField(
                header.value(),
                HttpFieldListRole::kSender) !=
            HttpFieldListParseStatus::kOk) {
            throw std::invalid_argument("invalid HTTP Connection header");
        }
    }
    return options;
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

// Finalize response-side HTTP/1 persistence after the runtime has folded in
// request-body completion and server policy. This is the sole protocol mutation:
// it honors an application-provided Connection: close and emits the version-
// appropriate Connection field. The returned plan is the authoritative transport
// lifecycle contract and retains the exact request version for head serialization.
[[nodiscard]] inline Http1ServerConnectionPlan http1FinalizeResponseConnection(
    HttpResponse& response,
    Http1ServerConnectionPlan plan) {
    const auto controlPlan = httpFinalResponseControlPlan(
        response,
        plan.protocolVersion());
    switch (controlPlan.status()) {
        case HttpFinalResponseControlStatus::kOk:
            break;
        case HttpFinalResponseControlStatus::kInvalidStatus:
            throw std::invalid_argument("invalid final HTTP response status");
        case HttpFinalResponseControlStatus::kInvalidUpgradeField:
            throw std::invalid_argument("invalid HTTP Upgrade header");
        case HttpFinalResponseControlStatus::kUpgradeRequired:
            throw std::invalid_argument("426 response requires an Upgrade protocol");
        case HttpFinalResponseControlStatus::kUpgradeUnavailable:
            throw std::invalid_argument("Upgrade is unavailable for this HTTP version");
    }
    const auto responseOptions = http1ResponseConnectionOptions(response);
    const auto& upgradeProtocols = controlPlan.upgradeProtocols();
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
    return plan;
}

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
    friend PreparedHttp1ResponseStream prepareHttp1ResponseStreamHead(
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

[[nodiscard]] inline PreparedHttp1ResponseStream prepareHttp1ResponseStreamHead(
    HttpResponse response,
    ResponseStreamKind kind,
    const Http1ResponseStreamPlan& plan,
    ResponseTrailerIntent trailerIntent) {
    const auto bodyPlan = httpResponseBodyPlan(plan.requestMethod(), response.status());
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
    const auto connectionPlan = http1FinalizeResponseConnection(
        response,
        plannedConnection);
    auto head = prepareResponseStreamHead(
        std::move(response), kind, plan.framing(), bodyPlan, trailerIntent);
    const auto responseHeadPlan =
        plan.framing() == ResponseStreamFraming::kHttp1Chunked
        ? http1ChunkedResponseStreamHeadPlan(
              head.commitPlan().bodyPlan(),
              connectionPlan)
        : http1CloseDelimitedResponseStreamHeadPlan(
              head.commitPlan().bodyPlan(),
              connectionPlan);
    return PreparedHttp1ResponseStream(
        std::move(head),
        responseHeadPlan,
        connectionPlan);
}

}  // namespace ruvia::detail
