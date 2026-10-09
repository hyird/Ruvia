#include <cstdint>
#include <limits>

#include "ruvia/http/http3_message_body.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"

#include "test_harness.h"

RUVIA_TEST(http3_message_body_accounts_data_and_fin_against_content_length) {
    ruvia::http3_message_body body(5, true);
    RUVIA_CHECK(body.feed(2, false) == ruvia::http3_message_body_result::accepted);
    RUVIA_CHECK_EQ(body.received_length(), 2U);
    RUVIA_CHECK(body.feed(3, true) == ruvia::http3_message_body_result::complete);
    RUVIA_CHECK(body.state() == ruvia::http3_message_body::state_type::complete);
    RUVIA_CHECK(body.feed(0, true) == ruvia::http3_message_body_result::already_complete);
}

RUVIA_TEST(http3_message_body_rejects_length_mismatch_excess_and_overflow) {
    ruvia::http3_message_body short_body(4, true);
    RUVIA_CHECK(short_body.feed(3, true) == ruvia::http3_message_body_result::content_length_mismatch);
    RUVIA_CHECK(short_body.state() == ruvia::http3_message_body::state_type::failed);
    RUVIA_CHECK(short_body.feed(0, true) == ruvia::http3_message_body_result::already_failed);

    ruvia::http3_message_body long_body(2, true);
    RUVIA_CHECK(long_body.feed(3, false) == ruvia::http3_message_body_result::content_length_exceeded);

    ruvia::http3_message_body overflow(std::nullopt, true);
    RUVIA_CHECK(overflow.feed(std::numeric_limits<std::uint64_t>::max(), false) ==
                ruvia::http3_message_body_result::accepted);
    RUVIA_CHECK(overflow.feed(1, false) == ruvia::http3_message_body_result::length_overflow);
}

RUVIA_TEST(http3_message_body_rejects_data_when_payload_is_suppressed) {
    const auto head_plan = ruvia::plan_http_response_body(ruvia::http_known_method::head, ruvia::http_status::ok);
    RUVIA_CHECK(head_plan.body_suppressed());
    ruvia::http3_message_body head(7, !head_plan.body_suppressed() && head_plan.status_allows_body());
    RUVIA_CHECK(head.feed(0, true) == ruvia::http3_message_body_result::complete);

    const auto no_content_plan = ruvia::plan_http_response_body(ruvia::http_known_method::get,
        ruvia::http_status::no_content);
    RUVIA_CHECK(!no_content_plan.status_allows_body());
    ruvia::http3_message_body no_content(std::nullopt,
        !no_content_plan.body_suppressed() && no_content_plan.status_allows_body());
    RUVIA_CHECK(no_content.feed(1, true) == ruvia::http3_message_body_result::payload_not_allowed);

    ruvia::http3_message_body empty_data(std::nullopt,
        !no_content_plan.body_suppressed() && no_content_plan.status_allows_body());
    RUVIA_CHECK(empty_data.feed(0, false) == ruvia::http3_message_body_result::accepted);
    RUVIA_CHECK(empty_data.feed(0, true) == ruvia::http3_message_body_result::complete);

    for (const auto status : {ruvia::http_status_code::from_value(100), ruvia::http_status::not_modified}) {
        const auto status_plan = ruvia::plan_http_response_body(ruvia::http_known_method::get, status);
        RUVIA_CHECK(!status_plan.status_allows_body());
        ruvia::http3_message_body status_body(9,
            !status_plan.body_suppressed() && status_plan.status_allows_body());
        RUVIA_CHECK(status_body.feed(0, true) == ruvia::http3_message_body_result::complete);
    }
}

RUVIA_TEST(http3_message_body_allows_head_content_length_without_data) {
    const auto plan = ruvia::plan_http_response_body(ruvia::http_known_method::head, ruvia::http_status::ok);
    ruvia::http3_message_body body(7, !plan.body_suppressed() && plan.status_allows_body());
    RUVIA_CHECK(body.feed(0, true) == ruvia::http3_message_body_result::complete);
}
