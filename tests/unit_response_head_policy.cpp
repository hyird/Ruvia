#include "test_harness.h"

#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/HttpResponse.h"

namespace {

using ruvia::detail::responseWritePolicy;

template <typename T>
concept HasValueSemanticResponseBodyPlan = requires(
    const T& plan,
    const T&& temporary) {
    { plan.bodyPlan() } ->
        std::same_as<ruvia::detail::HttpResponseBodyPlan>;
    { temporary.bodyPlan() } ->
        std::same_as<ruvia::detail::HttpResponseBodyPlan>;
};

static_assert(!std::default_initializable<
    ruvia::detail::Http1ResponseHeadPlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponsePlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponseHead>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1ChunkedResponseStreamHead>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1CloseDelimitedResponseStreamHead>);

template <typename BodyPlan>
concept AcceptsLooseBufferedResponseBodyPlan = requires(
    BodyPlan bodyPlan,
    const ruvia::HttpResponse& response) {
    ruvia::detail::httpBufferedResponseWritePlan(bodyPlan, response);
};

static_assert(!AcceptsLooseBufferedResponseBodyPlan<
    ruvia::detail::HttpResponseBodyPlan>);

template <typename T>
concept HasStaleHttp1BufferedWritePlanForwarder = requires(const T& plan) {
    plan.writePlan();
};

static_assert(!HasStaleHttp1BufferedWritePlanForwarder<
    ruvia::detail::Http1BufferedResponsePlan>);
static_assert(std::is_trivially_copyable_v<
    ruvia::detail::Http1BufferedResponsePlan>);
static_assert(
    sizeof(ruvia::detail::Http1BufferedResponsePlan) ==
    sizeof(ruvia::detail::Http1ResponseHeadPlan));

template <typename Plan>
concept HasValueSemanticResponseWritePolicy =
    requires(const Plan& plan) {
        { plan.policy() } ->
            std::same_as<ruvia::detail::ResponseWritePolicy>;
    } &&
    requires(const Plan&& plan) {
        { std::move(plan).policy() } ->
            std::same_as<ruvia::detail::ResponseWritePolicy>;
    };

static_assert(HasValueSemanticResponseWritePolicy<
    ruvia::detail::HttpResponseBodyPlan>);
static_assert(HasValueSemanticResponseWritePolicy<
    ruvia::detail::HttpBufferedResponseWritePlan>);
static_assert(HasValueSemanticResponseBodyPlan<
    ruvia::detail::HttpBufferedResponseWritePlan>);
static_assert(HasValueSemanticResponseBodyPlan<
    ruvia::detail::Http1ResponseHeadPlan>);
static_assert(std::is_trivially_copyable_v<
    ruvia::detail::HttpResponseBodyPlan>);
static_assert(sizeof(ruvia::detail::HttpResponseBodyPlan) <= 12);

}  // namespace

RUVIA_TEST(response_write_plan_unifies_method_status_and_body_size) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.body("hello");

    const auto getPlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kGet, response);
    RUVIA_CHECK(
        getPlan.requestMethod() == ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(
        getPlan.bodyPlan().requestMethod() ==
        ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(getPlan.matchesResponse(response));
    RUVIA_CHECK_EQ(getPlan.responseStatus(), std::uint16_t{200});
    RUVIA_CHECK_EQ(
        getPlan.bodyPlan().responseStatus(),
        std::uint16_t{200});
    RUVIA_CHECK(getPlan.statusAllowsBody());
    RUVIA_CHECK(
        getPlan.bodyPlan().contentSemantics() ==
        ruvia::detail::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(!getPlan.bodySuppressed());
    RUVIA_CHECK(getPlan.sendBody());
    RUVIA_CHECK_EQ(getPlan.contentLength(), static_cast<std::uint64_t>(5));

    const auto headPlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kHead, response);
    RUVIA_CHECK_EQ(headPlan.responseStatus(), std::uint16_t{200});
    RUVIA_CHECK(headPlan.bodyPlan().statusAllowsBody());
    RUVIA_CHECK(
        headPlan.bodyPlan().contentSemantics() ==
        ruvia::detail::HttpResponseContentSemantics::kWithoutContent);
    RUVIA_CHECK(headPlan.bodySuppressed());
    RUVIA_CHECK(!headPlan.sendBody());
    RUVIA_CHECK_EQ(headPlan.contentLength(), static_cast<std::uint64_t>(5));

    response.status(204);
    const auto noContentPlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kGet, response);
    RUVIA_CHECK_EQ(noContentPlan.responseStatus(), std::uint16_t{204});
    RUVIA_CHECK(!noContentPlan.bodyPlan().statusAllowsBody());
    RUVIA_CHECK(
        noContentPlan.bodyPlan().contentSemantics() ==
        ruvia::detail::HttpResponseContentSemantics::kWithoutContent);
    RUVIA_CHECK(noContentPlan.bodySuppressed());
    RUVIA_CHECK(!noContentPlan.sendBody());
    RUVIA_CHECK_EQ(noContentPlan.contentLength(), static_cast<std::uint64_t>(0));

    response.status(205);
    const auto resetContentPlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kGet, response);
    RUVIA_CHECK(!resetContentPlan.bodyPlan().statusAllowsBody());
    RUVIA_CHECK(
        resetContentPlan.bodyPlan().contentSemantics() ==
        ruvia::detail::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK(resetContentPlan.bodySuppressed());
    RUVIA_CHECK(!resetContentPlan.sendBody());
    RUVIA_CHECK_EQ(resetContentPlan.contentLength(), static_cast<std::uint64_t>(0));

    response.status(200);
    const auto connectPlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kConnect, response);
    RUVIA_CHECK(connectPlan.statusAllowsBody());
    RUVIA_CHECK(
        connectPlan.bodyPlan().contentSemantics() ==
        ruvia::detail::HttpResponseContentSemantics::kConnectTunnel);
    RUVIA_CHECK(connectPlan.bodySuppressed());
    RUVIA_CHECK(!connectPlan.sendBody());
    RUVIA_CHECK_EQ(connectPlan.contentLength(), static_cast<std::uint64_t>(0));
}

RUVIA_TEST(response_write_plan_rejects_mutated_response_snapshot) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::HttpResponse response(&resource);
    response.status(207);
    response.body("old");
    const auto plan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kGet,
        response);
    RUVIA_CHECK(plan.matchesResponse(response));

    response.body("longer");
    RUVIA_CHECK(!plan.matchesResponse(response));
    response.body("old");
    response.status(208);
    RUVIA_CHECK(!plan.matchesResponse(response));
}

RUVIA_TEST(response_policy_normal_status_allows_everything) {
    for (std::uint16_t status : {std::uint16_t{200}, std::uint16_t{206},
                                 std::uint16_t{404}, std::uint16_t{500}}) {
        const auto policy = responseWritePolicy(status);
        RUVIA_CHECK(policy.bodyAllowed());
        RUVIA_CHECK(policy.autoContentLengthAllowed());
        RUVIA_CHECK(policy.explicitContentLengthAllowed());
        RUVIA_CHECK(policy.transferEncodingAllowed());
    }
}

RUVIA_TEST(response_policy_bodyless_statuses_forbid_all_framing) {
    // 1xx informational and 204 are terminated by the empty line regardless of
    // headers (RFC 9112 §6.3 rule 1), so they carry no body and no framing headers.
    for (std::uint16_t status : {std::uint16_t{100}, std::uint16_t{101},
                                 std::uint16_t{199}, std::uint16_t{204}}) {
        const auto policy = responseWritePolicy(status);
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
    const auto policy = responseWritePolicy(205);
    RUVIA_CHECK(!policy.bodyAllowed());
    RUVIA_CHECK(policy.autoContentLengthAllowed());
    RUVIA_CHECK(!policy.explicitContentLengthAllowed());
    RUVIA_CHECK(!policy.transferEncodingAllowed());
}

RUVIA_TEST(response_policy_not_modified_keeps_explicit_content_length) {
    // 304 has no body, but may echo the Content-Length of the selected
    // representation; auto length and transfer-encoding stay forbidden.
    const auto policy = responseWritePolicy(304);
    RUVIA_CHECK(!policy.bodyAllowed());
    RUVIA_CHECK(!policy.autoContentLengthAllowed());
    RUVIA_CHECK(policy.explicitContentLengthAllowed());
    RUVIA_CHECK(!policy.transferEncodingAllowed());
}

RUVIA_TEST(http1_response_head_framing_is_an_exclusive_plan) {
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.body("hello");
    const auto bodyPlan = ruvia::detail::httpResponseBodyPlan(
        ruvia::HttpKnownMethod::kGet, 200);
    const auto connectionPlan =
        ruvia::detail::http1PlanHttp11RequestConnection(
            ruvia::detail::HttpConnectionOptions{});
    const auto writePlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpKnownMethod::kGet,
        response);
    const auto combined = ruvia::detail::http1BufferedResponsePlan(
        writePlan,
        connectionPlan);
    const auto& buffered = combined.headPlan();
    const auto chunked =
        ruvia::detail::http1ChunkedResponseStreamHeadPlan(
            bodyPlan,
            connectionPlan);
    const auto closeDelimited =
        ruvia::detail::http1CloseDelimitedResponseStreamHeadPlan(
            bodyPlan,
            connectionPlan);

    RUVIA_CHECK(buffered.buffered() != nullptr);
    RUVIA_CHECK(buffered.chunkedStream() == nullptr);
    RUVIA_CHECK(buffered.closeDelimitedStream() == nullptr);
    RUVIA_CHECK(chunked.buffered() == nullptr);
    RUVIA_CHECK(chunked.chunkedStream() != nullptr);
    RUVIA_CHECK(chunked.closeDelimitedStream() == nullptr);
    RUVIA_CHECK(closeDelimited.buffered() == nullptr);
    RUVIA_CHECK(closeDelimited.chunkedStream() == nullptr);
    RUVIA_CHECK(closeDelimited.closeDelimitedStream() != nullptr);
    RUVIA_CHECK(
        closeDelimited.bodyPlan().contentSemantics() ==
        ruvia::detail::HttpResponseContentSemantics::kWithContent);
    RUVIA_CHECK_EQ(
        buffered.buffered()->contentLength(),
        std::uint64_t{5});
    RUVIA_CHECK_EQ(combined.contentLength(), std::uint64_t{5});
    RUVIA_CHECK_EQ(combined.responseStatus(), std::uint16_t{200});
    RUVIA_CHECK(combined.sendBody());
    RUVIA_CHECK(
        combined.bodyPlan().requestMethod() == ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(
        buffered.protocolVersion() == ruvia::HttpProtocolVersion::kHttp11);

    RUVIA_CHECK_EQ(
        combined.contentLength(),
        combined.headPlan().buffered()->contentLength());
}
