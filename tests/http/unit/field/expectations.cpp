#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_expectations.h"

#include "test_harness.h"

namespace {

using ruvia::http_client_expectation_is_valid;
using ruvia::http_request_content_indication;
using ruvia::http_request_expectations;
using ruvia::http_unsupported_expectation_policy;

}  // namespace

// The Expect field and what a client that sends it must follow with.

RUVIA_TEST(client_expectation_requires_following_content) {
    RUVIA_CHECK(http_client_expectation_is_valid(false, http_request_content_indication::no_content));
    RUVIA_CHECK(!http_client_expectation_is_valid(true, http_request_content_indication::no_content));
    RUVIA_CHECK(http_client_expectation_is_valid(true, http_request_content_indication::will_follow));
}

RUVIA_TEST(expectations_parse_one_logical_recipient_list) {
    http_request_expectations expectations;
    expectations.parse_field(" , 100-continue, , 100-Continue, ");
    expectations.parse_field(" 100-CONTINUE ");

    RUVIA_CHECK(expectations.has_continue());
    RUVIA_CHECK(!expectations.has_unsupported());
    const auto no_content = expectations.server_plan(
        http_request_content_indication::no_content, http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(no_content.no_action() != nullptr);
    const auto with_content = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(with_content.send_continue() != nullptr);
}

RUVIA_TEST(expectations_preserve_unsupported_extensions_as_semantics) {
    http_request_expectations expectations;
    expectations.parse_field("100-continue");
    expectations.parse_field(R"(custom="a,b")");

    RUVIA_CHECK(expectations.has_continue());
    RUVIA_CHECK(expectations.has_unsupported());
    const auto rejected = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(rejected.rejection() != nullptr);
    if (const auto* rejection = rejected.rejection()) {
        RUVIA_CHECK_EQ(rejection->protocol_error().status(), ruvia::http_status::expectation_failed);
    }
    const auto ignored = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::ignore);
    RUVIA_CHECK(ignored.send_continue() != nullptr);

    expectations.ignore_continue();
    RUVIA_CHECK(!expectations.has_continue());
    RUVIA_CHECK(expectations.has_unsupported());
}
