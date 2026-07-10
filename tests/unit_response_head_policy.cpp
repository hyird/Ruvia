#include "test_harness.h"

#include <cstdint>
#include <memory_resource>

#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/detail/HttpResponseHeaderBits.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpTypes.h"

namespace {

using ruvia::detail::responseBodyFramingHeaderForbidden;
using ruvia::detail::responseHasForbiddenBodyFramingHeader;
using ruvia::detail::responseWritePolicy;

}  // namespace

RUVIA_TEST(response_write_plan_unifies_method_status_and_body_size) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.setBodyCopy("hello");

    const auto getPlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpMethod::kGet, response);
    RUVIA_CHECK(getPlan.statusAllowsBody());
    RUVIA_CHECK(!getPlan.bodySuppressed());
    RUVIA_CHECK(getPlan.sendBody());
    RUVIA_CHECK_EQ(getPlan.contentLength(), static_cast<std::uint64_t>(5));

    const auto headPlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpMethod::kHead, response);
    RUVIA_CHECK(headPlan.bodyPlan().statusAllowsBody());
    RUVIA_CHECK(headPlan.bodySuppressed());
    RUVIA_CHECK(!headPlan.sendBody());
    RUVIA_CHECK_EQ(headPlan.contentLength(), static_cast<std::uint64_t>(5));

    response.status(204);
    const auto noContentPlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpMethod::kGet, response);
    RUVIA_CHECK(!noContentPlan.bodyPlan().statusAllowsBody());
    RUVIA_CHECK(noContentPlan.bodySuppressed());
    RUVIA_CHECK(!noContentPlan.sendBody());
    RUVIA_CHECK_EQ(noContentPlan.contentLength(), static_cast<std::uint64_t>(0));
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

RUVIA_TEST(response_policy_reset_content_carries_framing) {
    // 205 (Reset Content) is NOT in RFC 9112 §6.3 rule 1, so it must declare its
    // (empty) body length: it uses the normal policy and receives an auto
    // Content-Length: 0 rather than being read until connection close.
    const auto policy = responseWritePolicy(205);
    RUVIA_CHECK(policy.bodyAllowed());
    RUVIA_CHECK(policy.autoContentLengthAllowed());
    RUVIA_CHECK(policy.explicitContentLengthAllowed());
    RUVIA_CHECK(policy.transferEncodingAllowed());
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

RUVIA_TEST(response_framing_header_forbidden_single_bit) {
    using ruvia::detail::kResponseHeaderContentLength;
    using ruvia::detail::kResponseHeaderContentType;
    using ruvia::detail::kResponseHeaderTransferEncoding;

    // Content-Length is forbidden only when explicit lengths are disallowed.
    RUVIA_CHECK(responseBodyFramingHeaderForbidden(kResponseHeaderContentLength, false, true));
    RUVIA_CHECK(!responseBodyFramingHeaderForbidden(kResponseHeaderContentLength, true, true));
    // Transfer-Encoding is forbidden only when transfer-encoding is disallowed.
    RUVIA_CHECK(responseBodyFramingHeaderForbidden(kResponseHeaderTransferEncoding, true, false));
    RUVIA_CHECK(!responseBodyFramingHeaderForbidden(kResponseHeaderTransferEncoding, true, true));
    // An unrelated header is never a forbidden framing header.
    RUVIA_CHECK(!responseBodyFramingHeaderForbidden(kResponseHeaderContentType, false, false));
}

RUVIA_TEST(response_has_forbidden_framing_header_over_bitmask) {
    using ruvia::detail::kResponseHeaderContentLength;
    using ruvia::detail::kResponseHeaderContentType;
    using ruvia::detail::kResponseHeaderTransferEncoding;

    // Under a 304 policy (explicit CL allowed, TE forbidden): a Content-Length
    // header is fine, a Transfer-Encoding header is forbidden.
    const auto notModified = responseWritePolicy(304);
    RUVIA_CHECK(!responseHasForbiddenBodyFramingHeader(
        kResponseHeaderContentLength,
        notModified.explicitContentLengthAllowed(),
        notModified.transferEncodingAllowed()));
    RUVIA_CHECK(responseHasForbiddenBodyFramingHeader(
        kResponseHeaderTransferEncoding,
        notModified.explicitContentLengthAllowed(),
        notModified.transferEncodingAllowed()));

    // Under a body-forbidden policy (204): both framing headers are forbidden.
    const auto noContent = responseWritePolicy(204);
    RUVIA_CHECK(responseHasForbiddenBodyFramingHeader(
        kResponseHeaderContentLength | kResponseHeaderContentType,
        noContent.explicitContentLengthAllowed(),
        noContent.transferEncodingAllowed()));
    RUVIA_CHECK(responseHasForbiddenBodyFramingHeader(
        kResponseHeaderTransferEncoding,
        noContent.explicitContentLengthAllowed(),
        noContent.transferEncodingAllowed()));

    // A normal (200) policy forbids nothing, and a bitmask without framing bits
    // is never forbidden.
    const auto normal = responseWritePolicy(200);
    RUVIA_CHECK(!responseHasForbiddenBodyFramingHeader(
        kResponseHeaderContentLength | kResponseHeaderTransferEncoding,
        normal.explicitContentLengthAllowed(),
        normal.transferEncodingAllowed()));
    RUVIA_CHECK(!responseHasForbiddenBodyFramingHeader(
        kResponseHeaderContentType, false, false));
}
