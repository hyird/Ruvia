#include <cstdint>
#include <limits>

#include "ruvia/http/Http3MessageBody.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpStatus.h"

#include "test_harness.h"

RUVIA_TEST(http3_message_body_accounts_data_and_fin_against_content_length) {
    ruvia::Http3MessageBody body(5, true);
    RUVIA_CHECK(body.feed(2, false) == ruvia::Http3MessageBodyResult::kAccepted);
    RUVIA_CHECK_EQ(body.receivedLength(), 2U);
    RUVIA_CHECK(body.feed(3, true) == ruvia::Http3MessageBodyResult::kComplete);
    RUVIA_CHECK(body.state() == ruvia::Http3MessageBody::State::kComplete);
    RUVIA_CHECK(body.feed(0, true) == ruvia::Http3MessageBodyResult::kAlreadyComplete);
}

RUVIA_TEST(http3_message_body_rejects_length_mismatch_excess_and_overflow) {
    ruvia::Http3MessageBody shortBody(4, true);
    RUVIA_CHECK(shortBody.feed(3, true) == ruvia::Http3MessageBodyResult::kContentLengthMismatch);
    RUVIA_CHECK(shortBody.state() == ruvia::Http3MessageBody::State::kFailed);
    RUVIA_CHECK(shortBody.feed(0, true) == ruvia::Http3MessageBodyResult::kAlreadyFailed);

    ruvia::Http3MessageBody longBody(2, true);
    RUVIA_CHECK(longBody.feed(3, false) == ruvia::Http3MessageBodyResult::kContentLengthExceeded);

    ruvia::Http3MessageBody overflow(std::nullopt, true);
    RUVIA_CHECK(overflow.feed(std::numeric_limits<std::uint64_t>::max(), false) ==
                ruvia::Http3MessageBodyResult::kAccepted);
    RUVIA_CHECK(overflow.feed(1, false) == ruvia::Http3MessageBodyResult::kLengthOverflow);
}

RUVIA_TEST(http3_message_body_rejects_data_when_payload_is_suppressed) {
    const auto headPlan = ruvia::planHttpResponseBody(ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk);
    RUVIA_CHECK(headPlan.bodySuppressed());
    ruvia::Http3MessageBody head(7, !headPlan.bodySuppressed() && headPlan.statusAllowsBody());
    RUVIA_CHECK(head.feed(0, true) == ruvia::Http3MessageBodyResult::kComplete);

    const auto noContentPlan = ruvia::planHttpResponseBody(ruvia::HttpKnownMethod::kGet,
        ruvia::http_status::kNoContent);
    RUVIA_CHECK(!noContentPlan.statusAllowsBody());
    ruvia::Http3MessageBody noContent(std::nullopt,
        !noContentPlan.bodySuppressed() && noContentPlan.statusAllowsBody());
    RUVIA_CHECK(noContent.feed(1, true) == ruvia::Http3MessageBodyResult::kPayloadNotAllowed);

    ruvia::Http3MessageBody emptyData(std::nullopt,
        !noContentPlan.bodySuppressed() && noContentPlan.statusAllowsBody());
    RUVIA_CHECK(emptyData.feed(0, false) == ruvia::Http3MessageBodyResult::kAccepted);
    RUVIA_CHECK(emptyData.feed(0, true) == ruvia::Http3MessageBodyResult::kComplete);

    for (const auto status : {ruvia::HttpStatusCode::fromValue(100), ruvia::http_status::kNotModified}) {
        const auto statusPlan = ruvia::planHttpResponseBody(ruvia::HttpKnownMethod::kGet, status);
        RUVIA_CHECK(!statusPlan.statusAllowsBody());
        ruvia::Http3MessageBody statusBody(9,
            !statusPlan.bodySuppressed() && statusPlan.statusAllowsBody());
        RUVIA_CHECK(statusBody.feed(0, true) == ruvia::Http3MessageBodyResult::kComplete);
    }
}

RUVIA_TEST(http3_message_body_allows_head_content_length_without_data) {
    const auto plan = ruvia::planHttpResponseBody(ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk);
    ruvia::Http3MessageBody body(7, !plan.bodySuppressed() && plan.statusAllowsBody());
    RUVIA_CHECK(body.feed(0, true) == ruvia::Http3MessageBodyResult::kComplete);
}
