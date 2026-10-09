#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ruvia/http/http_response.h"
#include "ruvia/web/error.h"
#include "ruvia/web/validation.h"

#include "http/http_error_response.h"
#include "test_harness.h"

namespace {

using ruvia::http_error_info;
using ruvia::detail::make_default_error_response;

[[nodiscard]] ruvia::http_response make_validation_error_response(
    std::pmr::memory_resource* resource, std::string_view field, std::string_view message) {
    ruvia::validator validator_value({.resource_ = resource});
    validator_value.add(field, "required", message);
    try {
        validator_value.throw_if_invalid({
            .status_ = ruvia::http_status::unprocessable_content,
            .code_ = "validation_failed",
            .message_ = "failed",
        });
    } catch (const ruvia::validation_error& error) {
        return make_default_error_response(resource, error.info());
    }
    throw std::logic_error("validator did not throw");
}

}  // namespace

RUVIA_TEST(default_error_response_escapes_message_in_json_body) {
    auto* resource = std::pmr::new_delete_resource();
    // A custom message carrying quotes must be JSON-escaped in the error body so
    // it cannot break out of the string and corrupt the response.
    http_error_info error({.status_ = ruvia::http_status::bad_request,
        .code_ = "bad_request",
        .message_ = "invalid \"input\""});
    const auto response = make_default_error_response(resource, error);

    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::bad_request);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/problem+json"));

    const auto body = response.body_bytes();
    RUVIA_CHECK(body.starts_with("{") && body.ends_with("}"));
    RUVIA_CHECK(body.find(R"("code":"bad_request")") != std::string_view::npos);
    RUVIA_CHECK(body.find(R"("detail":"invalid \"input\"")") != std::string_view::npos);
    RUVIA_CHECK(body.find(R"("type":"about:blank")") != std::string_view::npos);
    RUVIA_CHECK(body.find(R"("title":"Bad Request")") != std::string_view::npos);
    RUVIA_CHECK(body.find(R"("status":400)") != std::string_view::npos);
    RUVIA_CHECK(body.find(R"("instance":)") == std::string_view::npos);
    RUVIA_CHECK(body.find(R"("errors":)") == std::string_view::npos);
}

RUVIA_TEST(default_error_response_serializes_typed_validation_details) {
    auto* resource = std::pmr::new_delete_resource();
    const auto response = make_validation_error_response(resource, "x", "m");

    const auto body = response.body_bytes();
    RUVIA_CHECK(body.find(R"("errors":[{"field":"x","code":"required","message":"m"}])") !=
                std::string_view::npos);
}

RUVIA_TEST(default_error_response_escapes_typed_validation_details) {
    auto* resource = std::pmr::new_delete_resource();
    const auto response = make_validation_error_response(resource, "f\"x", "a\"b\\c");

    const auto body = response.body_bytes();
    RUVIA_CHECK(body.find(R"("field":"f\"x")") != std::string_view::npos);
    RUVIA_CHECK(body.find(R"("message":"a\"b\\c")") != std::string_view::npos);
}

RUVIA_TEST(default_error_response_does_not_set_transport_headers) {
    auto* resource = std::pmr::new_delete_resource();
    http_error_info error({.status_ = ruvia::http_status::internal_server_error,
        .code_ = "internal",
        .message_ = "boom"});
    const auto response = make_default_error_response(resource, error);
    RUVIA_CHECK(!response.header("Connection").has_value());
}

RUVIA_TEST(default_error_response_normalizes_non_error_status_and_status_text) {
    auto* resource = std::pmr::new_delete_resource();

    // An informational status is valid HTTP metadata but cannot terminate an
    // application error response, so the Web normalization maps it to 500.
    {
        const auto response = make_default_error_response(resource,
            http_error_info(
                {.status_ = ruvia::http_status::early_hints, .code_ = "x", .message_ = "y"}));
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::internal_server_error);
    }
    // Successful and redirection statuses are final responses, but they cannot
    // describe an application error either.
    for (const auto status : {ruvia::http_status::ok, ruvia::http_status::temporary_redirect}) {
        const auto response = make_default_error_response(
            resource, http_error_info({.status_ = status, .code_ = "x", .message_ = "y"}));
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::internal_server_error);
    }
    // A Web error label carrying CR/LF is replaced before JSON serialization.
    // It is presentation data only and never becomes an HTTP/1 reason phrase.
    {
        http_error_info error({.status_ = ruvia::http_status::bad_request,
            .code_ = "bad",
            .message_ = "msg",
            .status_text_ = std::string_view("Bad\r\nRequest", 12)});
        const auto response = make_default_error_response(resource, error);
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::bad_request);
        const auto body = response.body_bytes();
        RUVIA_CHECK(body.find(R"("title":"Bad Request")") != std::string_view::npos);
        RUVIA_CHECK(!(body.find('\r') != std::string_view::npos));
        RUVIA_CHECK(!(body.find('\n') != std::string_view::npos));
    }
    // An extension status has no conventional reason phrase. The Web JSON
    // envelope gets its own neutral label instead of inventing wire semantics.
    {
        const auto response = make_default_error_response(
            resource, http_error_info({.status_ = ruvia::http_status_code::from_value(599)}));
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status_code::from_value(599));
        const auto body = response.body_bytes();
        RUVIA_CHECK(body.find(R"("title":"HTTP Error")") != std::string_view::npos);
    }
    // about:blank titles describe the status, not application-specific labels.
    {
        const auto response = make_default_error_response(resource,
            http_error_info({.status_ = ruvia::http_status::bad_request,
                .message_ = "specific failure",
                .status_text_ = "Custom Label"}));
        const auto body = response.body_bytes();
        RUVIA_CHECK(body.find(R"("title":"Bad Request")") != std::string_view::npos);
        RUVIA_CHECK(body.find(R"("detail":"specific failure")") != std::string_view::npos);
    }
    // A valid in-range status is preserved unchanged.
    {
        const auto response = make_default_error_response(resource,
            http_error_info(
                {.status_ = ruvia::http_status::not_found, .code_ = "not_found", .message_ = "nope"}));
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::not_found);
    }
}

RUVIA_TEST(default_error_response_and_exception_copy_limit_validation_details) {
    ruvia::validator validator;
    validator.add("field", "required", "missing");
    std::pmr::vector<ruvia::validation_issue> issues;
    for (std::size_t index = 0; index < 2 * ruvia::max_validation_issues; ++index) {
        issues.push_back(ruvia::detail::validation_issue_access::copy(
            validator.issues()[0], issues.get_allocator().resource()));
    }
    const ruvia::validation_error error(issues);
    RUVIA_CHECK_EQ(error.issues().size(), ruvia::max_validation_issues);
    const auto response = make_default_error_response(std::pmr::get_default_resource(),
        http_error_info({.status_ = ruvia::http_status::bad_request, .validation_issues_ = issues}));
    const auto body = response.body_bytes();
    std::size_t count = 0;
    for (auto offset = body.find(R"("field":)"); offset != std::string_view::npos;
        offset = body.find(R"("field":)", offset + 1)) {
        ++count;
    }
    RUVIA_CHECK_EQ(count, ruvia::max_validation_issues);
}
