#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"

#include "test_harness.h"

namespace {

using ruvia::detail::responseWritePolicy;

}  // namespace

RUVIA_TEST(response_write_plan_unifies_method_status_and_body_size) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    response.body("hello");

    const auto getPlan =
        ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    RUVIA_CHECK(getPlan.requestMethod() == ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(getPlan.bodyPlan().requestMethod() == ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(getPlan.matchesResponse(response));
    RUVIA_CHECK_EQ(getPlan.responseStatus(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(getPlan.bodyPlan().responseStatus(), ruvia::http_status::kOk);
    RUVIA_CHECK(getPlan.statusAllowsBody());
    RUVIA_CHECK(getPlan.bodyPlan().contentSemantics() ==
                ruvia::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(!getPlan.bodySuppressed());
    RUVIA_CHECK(getPlan.sendBody());
    RUVIA_CHECK_EQ(getPlan.contentLength(), static_cast<std::uint64_t>(5));

    const auto headPlan =
        ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kHead, response);
    RUVIA_CHECK_EQ(headPlan.responseStatus(), ruvia::http_status::kOk);
    RUVIA_CHECK(headPlan.bodyPlan().statusAllowsBody());
    RUVIA_CHECK(headPlan.bodyPlan().contentSemantics() ==
                ruvia::HttpResponseContentSemantics::kWithoutContent);
    RUVIA_CHECK(headPlan.bodySuppressed());
    RUVIA_CHECK(!headPlan.sendBody());
    RUVIA_CHECK_EQ(headPlan.contentLength(), static_cast<std::uint64_t>(5));

    response.status(ruvia::http_status::kNoContent);
    const auto noContentPlan =
        ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    RUVIA_CHECK_EQ(noContentPlan.responseStatus(), ruvia::http_status::kNoContent);
    RUVIA_CHECK(!noContentPlan.bodyPlan().statusAllowsBody());
    RUVIA_CHECK(noContentPlan.bodyPlan().contentSemantics() ==
                ruvia::HttpResponseContentSemantics::kWithoutContent);
    RUVIA_CHECK(noContentPlan.bodySuppressed());
    RUVIA_CHECK(!noContentPlan.sendBody());
    RUVIA_CHECK_EQ(noContentPlan.contentLength(), static_cast<std::uint64_t>(0));

    response.status(ruvia::http_status::kResetContent);
    const auto resetContentPlan =
        ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    RUVIA_CHECK(!resetContentPlan.bodyPlan().statusAllowsBody());
    RUVIA_CHECK(resetContentPlan.bodyPlan().contentSemantics() ==
                ruvia::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(resetContentPlan.bodySuppressed());
    RUVIA_CHECK(!resetContentPlan.sendBody());
    RUVIA_CHECK_EQ(resetContentPlan.contentLength(), static_cast<std::uint64_t>(0));

    response.status(ruvia::http_status::kOk);
    const auto connectPlan =
        ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kConnect, response);
    RUVIA_CHECK(connectPlan.statusAllowsBody());
    RUVIA_CHECK(connectPlan.bodyPlan().contentSemantics() ==
                ruvia::HttpResponseContentSemantics::kConnectTunnel);
    RUVIA_CHECK(connectPlan.bodySuppressed());
    RUVIA_CHECK(!connectPlan.sendBody());
    RUVIA_CHECK_EQ(connectPlan.contentLength(), static_cast<std::uint64_t>(0));
}

RUVIA_TEST(response_body_plan_classifies_protocol_response_states) {
    const auto informational = ruvia::planHttpResponseBody(
        ruvia::HttpKnownMethod::kGet, ruvia::HttpStatusCode::fromValue(199));
    RUVIA_CHECK(informational.contentSemantics() ==
                ruvia::HttpResponseContentSemantics::kInformational);
    RUVIA_CHECK(informational.bodySuppressed());
    RUVIA_CHECK(!informational.statusAllowsBody());

    const auto protocolSwitch = ruvia::planHttpResponseBody(
        ruvia::HttpKnownMethod::kGet, ruvia::http_status::kSwitchingProtocols);
    RUVIA_CHECK(protocolSwitch.contentSemantics() ==
                ruvia::HttpResponseContentSemantics::kProtocolSwitch);
    RUVIA_CHECK(protocolSwitch.bodySuppressed());

    const auto connectTunnel = ruvia::planHttpResponseBody(
        ruvia::HttpKnownMethod::kConnect, ruvia::http_status::kOk);
    RUVIA_CHECK(connectTunnel.contentSemantics() ==
                ruvia::HttpResponseContentSemantics::kConnectTunnel);
    RUVIA_CHECK(connectTunnel.bodySuppressed());
    RUVIA_CHECK(connectTunnel.statusAllowsBody());

    const auto failedConnect = ruvia::planHttpResponseBody(
        ruvia::HttpKnownMethod::kConnect, ruvia::http_status::kBadRequest);
    RUVIA_CHECK(failedConnect.contentSemantics() ==
                ruvia::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(!failedConnect.bodySuppressed());
}

RUVIA_TEST(response_stream_plan_restricts_trailers_by_status_not_method) {
    for (const auto status : {ruvia::HttpStatusCode::fromValue(199),
             ruvia::http_status::kNoContent, ruvia::http_status::kNotModified}) {
        const auto plan = ruvia::httpResponseStreamCommitPlan(
            ruvia::ResponseStreamFraming::kHttp2Frames, ruvia::HttpKnownMethod::kGet,
            status, ruvia::ResponseTrailerIntent::kPresent);
        RUVIA_CHECK(!plan.trailerIntentAllowed());
    }

    const auto headPlan = ruvia::httpResponseStreamCommitPlan(
        ruvia::ResponseStreamFraming::kHttp2Frames, ruvia::HttpKnownMethod::kHead,
        ruvia::http_status::kOk, ruvia::ResponseTrailerIntent::kPresent);
    RUVIA_CHECK(headPlan.trailerIntentAllowed());
    RUVIA_CHECK(headPlan.headDisposition() == ruvia::ResponseStreamHeadDisposition::kTrailersOnly);
}

RUVIA_TEST(response_write_plan_rejects_mutated_response_snapshot) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kMultiStatus);
    response.body("old");
    const auto plan =
        ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    RUVIA_CHECK(plan.matchesResponse(response));

    response.body("longer");
    RUVIA_CHECK(!plan.matchesResponse(response));
    response.body("old");
    response.status(ruvia::http_status::kAlreadyReported);
    RUVIA_CHECK(!plan.matchesResponse(response));
}

RUVIA_TEST(response_policy_normal_status_allows_everything) {
    for (const ruvia::HttpStatusCode status :
        {ruvia::http_status::kOk, ruvia::http_status::kPartialContent,
            ruvia::http_status::kNotFound, ruvia::http_status::kInternalServerError}) {
        const auto policy = responseWritePolicy(status);
        RUVIA_CHECK(policy.normal() != nullptr);
        RUVIA_CHECK(policy.bodyForbidden() == nullptr);
        RUVIA_CHECK(policy.zeroLength() == nullptr);
        RUVIA_CHECK(policy.notModified() == nullptr);
        RUVIA_CHECK(policy.bodyAllowed());
        RUVIA_CHECK(policy.autoContentLengthAllowed());
        RUVIA_CHECK(policy.explicitContentLengthAllowed());
        RUVIA_CHECK(policy.transferEncodingAllowed());
    }
}

RUVIA_TEST(response_policy_bodyless_statuses_forbid_all_framing) {
    // 1xx informational and 204 are terminated by the empty line regardless of
    // headers (RFC 9112 §6.3 rule 1), so they carry no body and no framing headers.
    for (const ruvia::HttpStatusCode status :
        {ruvia::http_status::kContinue, ruvia::http_status::kSwitchingProtocols,
            ruvia::HttpStatusCode::fromValue(199), ruvia::http_status::kNoContent}) {
        const auto policy = responseWritePolicy(status);
        RUVIA_CHECK(policy.normal() == nullptr);
        RUVIA_CHECK(policy.bodyForbidden() != nullptr);
        RUVIA_CHECK(policy.zeroLength() == nullptr);
        RUVIA_CHECK(policy.notModified() == nullptr);
        RUVIA_CHECK(!policy.bodyAllowed());
        RUVIA_CHECK(!policy.autoContentLengthAllowed());
        RUVIA_CHECK(!policy.explicitContentLengthAllowed());
        RUVIA_CHECK(!policy.transferEncodingAllowed());
    }
}

RUVIA_TEST(response_policy_reset_content_owns_zero_length_framing) {
    // RFC 9110 §15.3.6 forbids content in 205. HTTP/1 does not infer a zero
    // length from that status, so the writer owns one canonical Content-Length:
    // 0 and rejects both caller-owned length and transfer coding declarations.
    const auto policy = responseWritePolicy(ruvia::http_status::kResetContent);
    RUVIA_CHECK(policy.normal() == nullptr);
    RUVIA_CHECK(policy.bodyForbidden() == nullptr);
    RUVIA_CHECK(policy.zeroLength() != nullptr);
    RUVIA_CHECK(policy.notModified() == nullptr);
    RUVIA_CHECK(!policy.bodyAllowed());
    RUVIA_CHECK(policy.autoContentLengthAllowed());
    RUVIA_CHECK(!policy.explicitContentLengthAllowed());
    RUVIA_CHECK(!policy.transferEncodingAllowed());
}

RUVIA_TEST(response_policy_not_modified_keeps_explicit_content_length) {
    // 304 has no body, but may echo the Content-Length of the selected
    // representation; auto length and transfer-encoding stay forbidden.
    const auto policy = responseWritePolicy(ruvia::http_status::kNotModified);
    RUVIA_CHECK(policy.normal() == nullptr);
    RUVIA_CHECK(policy.bodyForbidden() == nullptr);
    RUVIA_CHECK(policy.zeroLength() == nullptr);
    RUVIA_CHECK(policy.notModified() != nullptr);
    RUVIA_CHECK(!policy.bodyAllowed());
    RUVIA_CHECK(!policy.autoContentLengthAllowed());
    RUVIA_CHECK(policy.explicitContentLengthAllowed());
    RUVIA_CHECK(!policy.transferEncodingAllowed());
}

RUVIA_TEST(http1_response_head_framing_is_an_exclusive_plan) {
    ruvia::HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.body("hello");
    const auto bodyPlan =
        ruvia::planHttpResponseBody(ruvia::HttpKnownMethod::kGet, ruvia::http_status::kOk);
    const auto connectionPlan = ruvia::http1PlanHttp11RequestConnection(false);
    const auto writePlan =
        ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    const auto combined = ruvia::detail::http1BufferedResponsePlan(writePlan, connectionPlan);
    const auto& buffered = combined.headPlan();
    const auto chunked =
        ruvia::detail::http1ChunkedResponseStreamHeadPlan(bodyPlan, connectionPlan);
    const auto closeDelimited =
        ruvia::detail::http1CloseDelimitedResponseStreamHeadPlan(bodyPlan, connectionPlan);

    RUVIA_CHECK(buffered.buffered() != nullptr);
    RUVIA_CHECK(buffered.chunkedStream() == nullptr);
    RUVIA_CHECK(buffered.closeDelimitedStream() == nullptr);
    RUVIA_CHECK(chunked.buffered() == nullptr);
    RUVIA_CHECK(chunked.chunkedStream() != nullptr);
    RUVIA_CHECK(chunked.closeDelimitedStream() == nullptr);
    RUVIA_CHECK(closeDelimited.buffered() == nullptr);
    RUVIA_CHECK(closeDelimited.chunkedStream() == nullptr);
    RUVIA_CHECK(closeDelimited.closeDelimitedStream() != nullptr);
    RUVIA_CHECK(closeDelimited.bodyPlan().contentSemantics() ==
                ruvia::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK_EQ(buffered.buffered()->contentLength(), std::uint64_t{5});
    RUVIA_CHECK_EQ(combined.contentLength(), std::uint64_t{5});
    RUVIA_CHECK_EQ(combined.responseStatus(), ruvia::http_status::kOk);
    RUVIA_CHECK(combined.sendBody());
    RUVIA_CHECK(combined.bodyPlan().requestMethod() == ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(buffered.protocolVersion() == ruvia::HttpProtocolVersion::kHttp11);

    RUVIA_CHECK_EQ(combined.contentLength(), combined.headPlan().buffered()->contentLength());
}
