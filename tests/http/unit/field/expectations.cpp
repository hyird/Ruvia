#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_expectations.h"

#include "test_harness.h"

namespace {

using ruvia::http_client_expectation_is_valid;
using ruvia::http_request_content_indication;
using ruvia::http_request_expectations;
using ruvia::http_unsupported_expectation_policy;
using ruvia::detail::http_connection_options;
using ruvia::detail::http_field_list_parse_status;
using ruvia::detail::http_field_list_role;
using ruvia::detail::http_find_semicolon_parameter_ignore_case;
using ruvia::detail::http_find_semicolon_parameter_quoted_ignore_case;
using ruvia::detail::http_upgrade_protocols;
using ruvia::detail::is_valid_http_expect_field_value;
using ruvia::detail::is_valid_received_http_expect_field_value;

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

RUVIA_TEST(expect_field_value_validates_sender_syntax) {
    for (const std::string_view valid : {"100-continue", "custom=value", R"(custom="a,b")",
             R"(custom="quoted\"value"; name=token)", R"(custom = "x" ; name = "y")"}) {
        RUVIA_CHECK(is_valid_http_expect_field_value(valid));
    }

    for (const std::string_view invalid : {"", ",100-continue", "100-continue,", "bad value",
             "custom=", "custom=bad value", R"(custom="unterminated)", R"(custom="bad\)",
             "custom; name=value", "custom=value; bad-param"}) {
        RUVIA_CHECK(!is_valid_http_expect_field_value(invalid));
    }
}

RUVIA_TEST(received_expect_field_value_tolerates_empty_list_members_only) {
    RUVIA_CHECK(is_valid_received_http_expect_field_value(" , 100-continue, custom-feature, "));
    RUVIA_CHECK(is_valid_received_http_expect_field_value(""));

    for (const std::string_view invalid : {"bad value", "custom=", "custom=bad value",
             R"(custom="unterminated)", "custom; name=value"}) {
        RUVIA_CHECK(!is_valid_received_http_expect_field_value(invalid));
    }
}
