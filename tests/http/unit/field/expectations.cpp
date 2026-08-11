#include "test_harness.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/field/HttpExpectations.h"

namespace {

using ruvia::httpClientExpectationIsValid;
using ruvia::detail::HttpConnectionOptions;
using ruvia::detail::HttpFieldListParseStatus;
using ruvia::detail::HttpFieldListRole;
using ruvia::detail::httpFindSemicolonParameterIgnoreCase;
using ruvia::detail::httpFindSemicolonParameterQuotedIgnoreCase;
using ruvia::HttpRequestContentIndication;
using ruvia::HttpRequestExpectations;
using ruvia::HttpUnsupportedExpectationPolicy;
using ruvia::detail::HttpUpgradeProtocols;
using ruvia::detail::isValidHttpExpectFieldValue;
using ruvia::detail::isValidReceivedHttpExpectFieldValue;

}  // namespace

// The Expect field and what a client that sends it must follow with.

RUVIA_TEST(client_expectation_requires_following_content) {
    RUVIA_CHECK(httpClientExpectationIsValid(false, HttpRequestContentIndication::kNoContent));
    RUVIA_CHECK(!httpClientExpectationIsValid(true, HttpRequestContentIndication::kNoContent));
    RUVIA_CHECK(httpClientExpectationIsValid(true, HttpRequestContentIndication::kWillFollow));
}

RUVIA_TEST(expectations_parse_one_logical_recipient_list) {
    HttpRequestExpectations expectations;
    expectations.parseField(" , 100-continue, , 100-Continue, ");
    expectations.parseField(" 100-CONTINUE ");

    RUVIA_CHECK(expectations.hasContinue());
    RUVIA_CHECK(!expectations.hasUnsupported());
    const auto noContent = expectations.serverPlan(HttpRequestContentIndication::kNoContent, HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(noContent.noAction() != nullptr);
    const auto withContent = expectations.serverPlan(HttpRequestContentIndication::kWillFollow, HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(withContent.sendContinue() != nullptr);
}

RUVIA_TEST(expectations_preserve_unsupported_extensions_as_semantics) {
    HttpRequestExpectations expectations;
    expectations.parseField("100-continue");
    expectations.parseField(R"(custom="a,b")");

    RUVIA_CHECK(expectations.hasContinue());
    RUVIA_CHECK(expectations.hasUnsupported());
    const auto rejected = expectations.serverPlan(HttpRequestContentIndication::kWillFollow, HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(rejected.rejection() != nullptr);
    if (const auto* rejection = rejected.rejection()) {
        RUVIA_CHECK_EQ(rejection->protocolError().status(), ruvia::http_status::kExpectationFailed);
    }
    const auto ignored = expectations.serverPlan(HttpRequestContentIndication::kWillFollow, HttpUnsupportedExpectationPolicy::kIgnore);
    RUVIA_CHECK(ignored.sendContinue() != nullptr);

    expectations.ignoreContinue();
    RUVIA_CHECK(!expectations.hasContinue());
    RUVIA_CHECK(expectations.hasUnsupported());
}

RUVIA_TEST(expect_field_value_validates_sender_syntax) {
    for (const std::string_view valid : {"100-continue", "custom=value", R"(custom="a,b")", R"(custom="quoted\"value"; name=token)", R"(custom = "x" ; name = "y")"}) {
        RUVIA_CHECK(isValidHttpExpectFieldValue(valid));
    }

    for (const std::string_view invalid : {"", ",100-continue", "100-continue,", "bad value", "custom=", "custom=bad value", R"(custom="unterminated)", R"(custom="bad\)", "custom; name=value", "custom=value; bad-param"}) {
        RUVIA_CHECK(!isValidHttpExpectFieldValue(invalid));
    }
}

RUVIA_TEST(received_expect_field_value_tolerates_empty_list_members_only) {
    RUVIA_CHECK(isValidReceivedHttpExpectFieldValue(" , 100-continue, custom-feature, "));
    RUVIA_CHECK(isValidReceivedHttpExpectFieldValue(""));

    for (const std::string_view invalid : {"bad value", "custom=", "custom=bad value", R"(custom="unterminated)", "custom; name=value"}) {
        RUVIA_CHECK(!isValidReceivedHttpExpectFieldValue(invalid));
    }
}
