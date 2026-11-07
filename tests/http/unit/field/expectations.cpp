#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_expectations.h"

namespace {

using ruvia::http_client_expectation_is_valid;
using ruvia::http_request_content_indication;
using ruvia::http_request_expectations;
using ruvia::http_unsupported_expectation_policy;

}  // namespace

// The Expect field and what a client that sends it must follow with.

TEST(Ruvia, client_expectation_requires_following_content) {
    EXPECT_TRUE(http_client_expectation_is_valid(false, http_request_content_indication::no_content));
    EXPECT_TRUE(!http_client_expectation_is_valid(true, http_request_content_indication::no_content));
    EXPECT_TRUE(http_client_expectation_is_valid(true, http_request_content_indication::will_follow));
}

TEST(Ruvia, expectations_parse_one_logical_recipient_list) {
    http_request_expectations expectations;
    expectations.parse_field(" , 100-continue, , 100-Continue, ");
    expectations.parse_field(" 100-CONTINUE ");

    EXPECT_TRUE(expectations.has_continue());
    EXPECT_TRUE(!expectations.has_unsupported());
    const auto no_content = expectations.server_plan(
        http_request_content_indication::no_content, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(no_content == ruvia::http_server_expectation_plan::no_action);
    const auto with_content = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(with_content == ruvia::http_server_expectation_plan::send_continue);
}

TEST(Ruvia, expectations_preserve_unsupported_extensions_as_semantics) {
    http_request_expectations expectations;
    expectations.parse_field("100-continue");
    expectations.parse_field(R"(custom="a,b")");

    EXPECT_TRUE(expectations.has_continue());
    EXPECT_TRUE(expectations.has_unsupported());
    const auto rejected = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(rejected == ruvia::http_server_expectation_plan::reject_unsupported);
    EXPECT_EQ(ruvia::http_unsupported_expectation_error().status(), ruvia::http_status::expectation_failed);
    const auto ignored = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::ignore);
    EXPECT_TRUE(ignored == ruvia::http_server_expectation_plan::send_continue);

    expectations.ignore_continue();
    EXPECT_TRUE(!expectations.has_continue());
    EXPECT_TRUE(expectations.has_unsupported());
}
