#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/web/detail/server/response/HttpServerResponseState.h"

namespace {

using ruvia::HttpResponse;

ruvia::detail::Http1ServerConnectionPlan connectionPlanFor(
    ruvia::HttpProtocolVersion protocolVersion) {
    const ruvia::detail::HttpConnectionOptions options;
    return protocolVersion == ruvia::HttpProtocolVersion::kHttp10
               ? ruvia::detail::http1PlanHttp10RequestConnection(options)
               : ruvia::detail::http1PlanHttp11RequestConnection(options);
}

ruvia::detail::PreparedHttp1ResponseStream prepareStream(HttpResponse response,
    ruvia::detail::ResponseStreamKind kind, const ruvia::detail::Http1ResponseStreamPlan& plan,
    ruvia::detail::ResponseTrailerIntent trailerIntent) {
    auto result = ruvia::detail::prepareHttp1ResponseStreamHead(
        std::move(response), kind, plan, trailerIntent);
    auto* prepared = result.prepared();
    if (result.failure() != nullptr || prepared == nullptr) {
        throw std::logic_error("expected prepared HTTP/1 response stream");
    }
    return std::move(*prepared);
}

template <typename Fn>
bool throwsInvalid(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(finalize_buffered_response_preserves_request_version_and_persistence) {
    using ruvia::Http1ClosePolicy;
    using ruvia::detail::finalizeBufferedRouteResponse;
    using ruvia::detail::Http1ServerRequestParser;

    Http1ServerRequestParser parser;
    const auto finalize = [&](std::string_view request) {
        HttpResponse response({.resource = std::pmr::new_delete_resource()});
        response.status(ruvia::http_status::kOk);
        ruvia::detail::Http1RequestSequence requestSequence(std::nullopt);
        const auto plan = finalizeBufferedRouteResponse(
            response, parser.parseMessage(request).connectionPlan, requestSequence);
        return std::pair(plan.disposition(),
            std::string(response.header("Connection").value_or(std::string_view{})));
    };

    // RFC 9112 §9.3: a kept-alive HTTP/1.0 response MUST advertise keep-alive,
    // otherwise the client (which defaults to close) never reuses the connection.
    const auto http10Reuse = finalize("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    RUVIA_CHECK(http10Reuse.first == Http1ClosePolicy::kAllowReuse);
    RUVIA_CHECK_EQ(http10Reuse.second, std::string("keep-alive"));
    // HTTP/1.1 is persistent by default -> no Connection header needed.
    const auto http11Reuse = finalize("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(http11Reuse.first == Http1ClosePolicy::kAllowReuse);
    RUVIA_CHECK_EQ(http11Reuse.second, std::string(""));
    // A non-kept-alive response is closed regardless of version.
    RUVIA_CHECK_EQ(finalize("GET / HTTP/1.0\r\n\r\n").second, std::string("close"));
    RUVIA_CHECK_EQ(finalize("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n").second,
        std::string("close"));
}

RUVIA_TEST(http1_final_response_commit_requirement_preserves_typed_failure) {
    using ruvia::detail::Http1FinalResponseCommitError;
    using ruvia::detail::Http1ServerRequestParser;

    Http1ServerRequestParser parser;
    const auto requestPlan =
        parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan;

    HttpResponse propagated({.resource = std::pmr::new_delete_resource()});
    propagated.status(ruvia::http_status::kUpgradeRequired);
    bool caughtTypedFailure = false;
    try {
        (void)ruvia::detail::requireHttp1FinalResponseCommit(propagated, requestPlan);
    } catch (const Http1FinalResponseCommitError& error) {
        caughtTypedFailure = std::string_view(error.what()) ==
                             "Upgrade Required response requires an Upgrade protocol";
    }
    RUVIA_CHECK(caughtTypedFailure);
}

RUVIA_TEST(http1_buffered_request_limit_closes_the_typed_connection_plan) {
    using ruvia::Http1ClosePolicy;
    using ruvia::detail::finalizeBufferedRouteResponse;
    using ruvia::detail::Http1RequestSequence;
    using ruvia::detail::Http1ServerRequestParser;

    Http1ServerRequestParser parser;
    const auto requestPlan =
        parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan;
    Http1RequestSequence requestSequence(std::optional<std::size_t>{5});
    for (std::size_t completed = 0; completed < 4; ++completed) {
        HttpResponse response({.resource = std::pmr::new_delete_resource()});
        const auto connectionPlan =
            finalizeBufferedRouteResponse(response, requestPlan, requestSequence);
        RUVIA_CHECK(connectionPlan.disposition() == Http1ClosePolicy::kAllowReuse);
        RUVIA_CHECK(!response.header("Connection").has_value());
    }

    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    const auto connectionPlan =
        finalizeBufferedRouteResponse(response, requestPlan, requestSequence);
    RUVIA_CHECK(connectionPlan.disposition() == Http1ClosePolicy::kCloseAfterResponse);
    RUVIA_CHECK_EQ(std::string(response.header("Connection").value_or(std::string_view{})),
        std::string("close"));
}

RUVIA_TEST(http1_request_sequence_rejects_configured_zero_budget) {
    RUVIA_CHECK(throwsInvalid(
        [] { ruvia::detail::Http1RequestSequence sequence(std::optional<std::size_t>{0}); }));
}

RUVIA_TEST(http1_request_sequence_unifies_buffered_and_committed_completion) {
    using ruvia::Http1ClosePolicy;
    using ruvia::detail::http1PlanResponseStream;
    using ruvia::detail::Http1RequestSequence;
    using ruvia::detail::Http1ServerRequestParser;
    using ruvia::detail::ResponseStreamKind;
    using ruvia::detail::ResponseTrailerIntent;

    const auto reusablePlan = connectionPlanFor(ruvia::HttpProtocolVersion::kHttp11);
    Http1RequestSequence requestSequence(std::optional<std::size_t>{2});
    RUVIA_CHECK(requestSequence.nextResponseClosePolicy() == Http1ClosePolicy::kAllowReuse);

    const auto firstPlan = requestSequence.completeUncommittedResponse(reusablePlan);
    RUVIA_CHECK(firstPlan.disposition() == Http1ClosePolicy::kAllowReuse);
    RUVIA_CHECK(requestSequence.nextResponseClosePolicy() == Http1ClosePolicy::kCloseAfterResponse);

    Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto streamPlan =
        http1PlanResponseStream(parsed, requestSequence.nextResponseClosePolicy());
    RUVIA_CHECK(streamPlan.closePolicy() == Http1ClosePolicy::kCloseAfterResponse);
    HttpResponse streamResponse({.resource = std::pmr::new_delete_resource()});
    const auto prepared = prepareStream(std::move(streamResponse), ResponseStreamKind::kGeneric,
        streamPlan, ResponseTrailerIntent::kNone);
    RUVIA_CHECK(prepared.connectionPlan().disposition() == Http1ClosePolicy::kCloseAfterResponse);
    requestSequence.completeCommittedResponse(prepared.connectionPlan());

    Http1RequestSequence unlimited(std::nullopt);
    unlimited.completeCommittedResponse(reusablePlan);
    RUVIA_CHECK(unlimited.nextResponseClosePolicy() == Http1ClosePolicy::kAllowReuse);
}

RUVIA_TEST(http1_body_completion_tightens_without_losing_protocol_version) {
    using ruvia::Http1ClosePolicy;
    using ruvia::Http1RequestBodyConsumption;
    using ruvia::detail::finalizeBodyRouteResponse;
    using ruvia::detail::Http1ServerRequestParser;

    Http1ServerRequestParser parser;
    constexpr std::string_view request =
        "POST / HTTP/1.0\r\nConnection: keep-alive\r\nContent-Length: 1\r\n\r\nx";

    HttpResponse completeResponse({.resource = std::pmr::new_delete_resource()});
    completeResponse.body("response");
    ruvia::detail::Http1RequestSequence completeRequestSequence(std::nullopt);
    const auto completePlan =
        finalizeBodyRouteResponse(completeResponse, parser.parseMessage(request).connectionPlan,
            completeRequestSequence, Http1RequestBodyConsumption::kComplete);
    RUVIA_CHECK(completePlan.disposition() == Http1ClosePolicy::kAllowReuse);
    RUVIA_CHECK(completePlan.protocolVersion() == ruvia::HttpProtocolVersion::kHttp10);
    RUVIA_CHECK_EQ(std::string(completeResponse.header("Connection").value_or(std::string_view{})),
        std::string("keep-alive"));

    HttpResponse incompleteResponse({.resource = std::pmr::new_delete_resource()});
    incompleteResponse.body("response");
    ruvia::detail::Http1RequestSequence incompleteRequestSequence(std::nullopt);
    const auto incompletePlan =
        finalizeBodyRouteResponse(incompleteResponse, parser.parseMessage(request).connectionPlan,
            incompleteRequestSequence, Http1RequestBodyConsumption::kIncomplete);
    RUVIA_CHECK(incompletePlan.disposition() == Http1ClosePolicy::kCloseAfterResponse);
    RUVIA_CHECK(incompletePlan.protocolVersion() == ruvia::HttpProtocolVersion::kHttp10);
    RUVIA_CHECK_EQ(
        std::string(incompleteResponse.header("Connection").value_or(std::string_view{})),
        std::string("close"));
}

