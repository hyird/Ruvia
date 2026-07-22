#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/http/error/HttpErrorResponse.h"

namespace {

using ruvia::HttpErrorInfo;
using ruvia::detail::makeDefaultErrorResponse;

}  // namespace

RUVIA_TEST(default_error_response_escapes_message_in_json_body) {
    auto* resource = std::pmr::new_delete_resource();
    // A custom message carrying quotes must be JSON-escaped in the error body so
    // it cannot break out of the string and corrupt the response.
    HttpErrorInfo error(
        ruvia::http_status::kBadRequest,
        "bad_request",
        "invalid \"input\"");
    const auto response = makeDefaultErrorResponse(resource, error);

    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));

    const auto body = ruvia::detail::responseBody(response).bytes();
    RUVIA_CHECK(body.starts_with("{") && body.ends_with("}"));
    RUVIA_CHECK(body.find(R"("code":"bad_request")") != std::string_view::npos);
    RUVIA_CHECK(body.find(R"("message":"invalid \"input\"")") != std::string_view::npos);
}

RUVIA_TEST(default_error_response_embeds_details_json) {
    auto* resource = std::pmr::new_delete_resource();
    HttpErrorInfo error(
        ruvia::http_status::kUnprocessableContent,
        "validation_failed",
        "failed",
        "Unprocessable Content",
        R"([{"field":"x","code":"required","message":"m"}])");
    const auto response = makeDefaultErrorResponse(resource, error);

    const auto body = ruvia::detail::responseBody(response).bytes();
    // The already-valid details JSON is embedded verbatim under "details".
    RUVIA_CHECK(
        body.find(R"("details":[{"field":"x","code":"required","message":"m"}])") !=
        std::string_view::npos);
}

RUVIA_TEST(default_error_response_does_not_set_transport_headers) {
    auto* resource = std::pmr::new_delete_resource();
    HttpErrorInfo error(
        ruvia::http_status::kInternalServerError, "internal", "boom");
    const auto response = makeDefaultErrorResponse(resource, error);
    RUVIA_CHECK(!response.header("Connection").has_value());
}

RUVIA_TEST(default_error_response_normalizes_non_error_status_and_status_text) {
    auto* resource = std::pmr::new_delete_resource();

    // An informational status is valid HTTP metadata but cannot terminate an
    // application error response, so the Web normalization maps it to 500.
    {
        const auto response = makeDefaultErrorResponse(resource, HttpErrorInfo(ruvia::http_status::kEarlyHints, "x", "y"));
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kInternalServerError);
    }
    // Successful and redirection statuses are final responses, but they cannot
    // describe an application error either.
    for (const auto status : {
             ruvia::http_status::kOk,
             ruvia::http_status::kTemporaryRedirect}) {
        const auto response =
            makeDefaultErrorResponse(resource, HttpErrorInfo(status, "x", "y"));
        RUVIA_CHECK_EQ(
            response.status(), ruvia::http_status::kInternalServerError);
    }
    // A Web error label carrying CR/LF is replaced before JSON serialization.
    // It is presentation data only and never becomes an HTTP/1 reason phrase.
    {
        HttpErrorInfo error(
            ruvia::http_status::kBadRequest,
            "bad",
            "msg",
            std::string_view("Bad\r\nRequest", 12));
        const auto response = makeDefaultErrorResponse(resource, error);
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kBadRequest);
        const auto body = ruvia::detail::responseBody(response).bytes();
        RUVIA_CHECK(body.find(R"("error":"Bad Request")") != std::string_view::npos);
        RUVIA_CHECK(body.find('\r') == std::string_view::npos);
        RUVIA_CHECK(body.find('\n') == std::string_view::npos);
    }
    // An extension status has no conventional reason phrase. The Web JSON
    // envelope gets its own neutral label instead of inventing wire semantics.
    {
        const auto response = makeDefaultErrorResponse(resource, HttpErrorInfo(ruvia::HttpStatusCode::fromValue(599)));
        RUVIA_CHECK_EQ(response.status(), ruvia::HttpStatusCode::fromValue(599));
        const auto body = ruvia::detail::responseBody(response).bytes();
        RUVIA_CHECK(body.find(R"("error":"HTTP Error")") != std::string_view::npos);
    }
    // A valid in-range status is preserved unchanged.
    {
        const auto response = makeDefaultErrorResponse(resource, HttpErrorInfo(ruvia::http_status::kNotFound, "not_found", "nope"));
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kNotFound);
    }
}
